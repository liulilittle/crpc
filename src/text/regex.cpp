// CRPC -- backtracking regular expression engine.
// See include/crpc/text/regex.hpp.
//
// The engine has three stages:
//
//   1. a recursive-descent parser producing a small syntax tree; it is the only
//      recursive component, and the group-depth limit bounds its stack use;
//   2. an emitter that flattens the tree into a linear instruction list;
//   3. a matcher that runs the instructions with its own backtrack stack.
//
// The matcher's stack is a heap vector, never the C++ stack, so a long subject
// cannot overflow it. Every executed instruction consumes one step from
// `regex_options::max_steps`; that budget, not a wall clock, is what stops a
// catastrophic pattern such as `(a+)+b`.
//
// Two behaviours follow Python's `re`, which is the source of the expectation
// vectors in the test suite:
//
//   * a repetition whose body matches empty stops repeating but keeps the
//     captures of that last, empty iteration (`(a?)*` on "aa" leaves group 1 at
//     [2,2]);
//   * `$` with `multiline` off also matches just before a single trailing
//     newline.
#include <crpc/text/regex.hpp>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace crpc {
namespace text {

namespace {

constexpr std::size_t unbounded = static_cast<std::size_t>(-1);

// Group nesting cap. Capturing groups are already capped by
// regex_max_groups, but `(?:...)` is not, and the parser is the only recursive
// stage: this bound is what keeps a generated "((((...))))" from exhausting
// the C++ stack.
constexpr std::size_t max_nesting = 64;

// --- Byte helpers ------------------------------------------------------------

unsigned char fold_byte(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z')
               ? static_cast<unsigned char>(c + ('a' - 'A'))
               : c;
}

unsigned char fold_if(unsigned char c, bool icase) noexcept {
    return icase ? fold_byte(c) : c;
}

bool is_word_byte(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

bool is_digit_byte(unsigned char c) noexcept {
    return c >= '0' && c <= '9';
}

bool is_space_byte(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

bool is_ascii_alnum(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

// A 256-bit set of bytes. A negated class is materialised as its complement,
// so matching is one bit test with no "negated" flag to remember.
struct byte_set {
    std::array<std::uint8_t, 32> bits{};

    void add(unsigned char c) noexcept {
        bits[c >> 3] = static_cast<std::uint8_t>(
            bits[c >> 3] | static_cast<std::uint8_t>(1u << (c & 7u)));
    }

    bool contains(unsigned char c) const noexcept {
        return ((bits[c >> 3] >> (c & 7u)) & 1u) != 0;
    }

    void unite(const byte_set& other) noexcept {
        for (std::size_t i = 0; i < bits.size(); ++i) {
            bits[i] = static_cast<std::uint8_t>(bits[i] | other.bits[i]);
        }
    }

    void invert() noexcept {
        for (std::uint8_t& byte : bits) {
            byte = static_cast<std::uint8_t>(~byte);
        }
    }
};

// --- Failures ----------------------------------------------------------------

status syntax_error(const char* what, std::size_t offset) {
    std::string message = "regex: ";
    message += what;
    message += " at offset ";
    message += std::to_string(offset);
    return status(rpc_code::decode_error, std::move(message));
}

// Returned as an rpc_error rather than a status: both `status` and
// `result<T>` accept one implicitly, so every stage can return it directly.
rpc_error limit_error(const char* what) {
    return rpc_error{rpc_code::resource_exhausted,
                     std::string("regex: ") + what};
}

// --- Instructions ------------------------------------------------------------

enum class opcode : std::uint8_t {
    byte,               // a = byte (already folded when icase)
    any,                // '.'
    char_class,         // a = index into impl::classes
    split,              // a = preferred pc, b = alternative pc
    jump,               // a = target pc
    save,               // a = slot (capture group start/end; 0/1 = match)
    mark,               // a = slot (iteration start position)
    progress,           // a = slot, b = loop head: repeat only on progress
    count_init,         // a = slot: iteration counter = 0
    count_add,          // a = slot: ++counter
    count_ge_jump,      // a = slot, b = bound, c = target: jump when >= bound
    fail,               // backtrack unconditionally
    begin_line,         // '^'
    end_line,           // '$'
    word_boundary,      // '\b'
    not_word_boundary,  // '\B'
    group_ref,          // a = capture group number
    accept
};

struct instruction {
    opcode code;
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t c;
};

// --- Syntax tree -------------------------------------------------------------

struct ast_node {
    enum class kind : std::uint8_t {
        empty,
        byte,
        any,
        char_class,
        begin_line,
        end_line,
        word_boundary,
        not_word_boundary,
        backref,
        concat,
        alternate,
        repeat,
        group
    };

    kind type = kind::empty;
    unsigned char byte_value = 0;
    std::uint32_t value = 0;  // class index or capture group number
    std::size_t min = 0;
    std::size_t max = 0;
    bool lazy = false;
    std::vector<std::size_t> children;
};

struct ast_tree {
    std::vector<ast_node> nodes;
    std::vector<byte_set> classes;
};

// --- Parser ------------------------------------------------------------------

class pattern_parser {
public:
    pattern_parser(std::string_view source, const regex_options& options,
                   ast_tree& tree)
        : source_(source), options_(options), tree_(tree) {}

    status parse(std::size_t& root) {
        status parsed = parse_alternation(root);
        if (!parsed.ok()) {
            return parsed;
        }
        if (pos_ != source_.size()) {
            return syntax_error("unmatched ')'", pos_);
        }
        return {};
    }

    std::size_t group_count() const noexcept { return group_count_; }

private:
    status parse_alternation(std::size_t& out) {
        std::size_t first = 0;
        status parsed = parse_sequence(first);
        if (!parsed.ok()) {
            return parsed;
        }
        if (peek() != '|') {
            out = first;
            return {};
        }
        ast_node node;
        node.type = ast_node::kind::alternate;
        node.children.push_back(first);
        while (peek() == '|') {
            ++pos_;
            std::size_t branch = 0;
            parsed = parse_sequence(branch);
            if (!parsed.ok()) {
                return parsed;
            }
            node.children.push_back(branch);
        }
        out = add(std::move(node));
        return {};
    }

    status parse_sequence(std::size_t& out) {
        ast_node node;
        node.type = ast_node::kind::concat;
        while (pos_ < source_.size() && source_[pos_] != '|' &&
               source_[pos_] != ')') {
            std::size_t item = 0;
            const status parsed = parse_repeat(item);
            if (!parsed.ok()) {
                return parsed;
            }
            node.children.push_back(item);
        }
        if (node.children.size() == 1) {
            out = node.children.front();
            return {};
        }
        out = add(std::move(node));
        return {};
    }

    status parse_repeat(std::size_t& out) {
        bool quantifiable = true;
        std::size_t atom = 0;
        status parsed = parse_atom(atom, quantifiable);
        if (!parsed.ok()) {
            return parsed;
        }
        const char next = peek();
        if (next != '*' && next != '+' && next != '?' && next != '{') {
            out = atom;
            return {};
        }
        if (!quantifiable) {
            return syntax_error("a quantifier cannot apply to an assertion",
                                pos_);
        }
        std::size_t min = 0;
        std::size_t max = 0;
        bool lazy = false;
        parsed = parse_quantifier(min, max, lazy);
        if (!parsed.ok()) {
            return parsed;
        }
        const char after = peek();
        if (after == '*' || after == '+' || after == '?' || after == '{') {
            return syntax_error("multiple repeat", pos_);
        }
        ast_node node;
        node.type = ast_node::kind::repeat;
        node.min = min;
        node.max = max;
        node.lazy = lazy;
        node.children.push_back(atom);
        out = add(std::move(node));
        return {};
    }

    status parse_quantifier(std::size_t& min, std::size_t& max, bool& lazy) {
        const char c = source_[pos_];
        if (c == '*') {
            ++pos_;
            min = 0;
            max = unbounded;
        } else if (c == '+') {
            ++pos_;
            min = 1;
            max = unbounded;
        } else if (c == '?') {
            ++pos_;
            min = 0;
            max = 1;
        } else {
            const std::size_t brace = pos_;
            ++pos_;
            status parsed = read_count(min, brace);
            if (!parsed.ok()) {
                return parsed;
            }
            if (peek() == '}') {
                ++pos_;
                max = min;
            } else if (peek() == ',') {
                ++pos_;
                if (peek() == '}') {
                    ++pos_;
                    max = unbounded;
                } else {
                    parsed = read_count(max, brace);
                    if (!parsed.ok()) {
                        return parsed;
                    }
                    if (peek() != '}') {
                        return syntax_error("unterminated '{' quantifier",
                                            brace);
                    }
                    ++pos_;
                    if (max < min) {
                        return syntax_error(
                            "repeat upper bound is smaller than lower bound",
                            brace);
                    }
                }
            } else {
                return syntax_error("unterminated '{' quantifier", brace);
            }
        }
        if (peek() == '?') {
            ++pos_;
            lazy = true;
        }
        return {};
    }

    // A missing digit is a syntax error; a count above regex_max_repeat is a
    // resource limit (it would otherwise expand into that many instructions).
    status read_count(std::size_t& value, std::size_t brace) {
        const std::size_t begin = pos_;
        value = 0;
        while (pos_ < source_.size() && source_[pos_] >= '0' &&
               source_[pos_] <= '9') {
            const std::size_t digit =
                static_cast<std::size_t>(source_[pos_] - '0');
            if (value > (regex_max_repeat - digit) / 10) {
                return limit_error("repeat count is larger than the limit");
            }
            value = value * 10 + digit;
            ++pos_;
        }
        if (pos_ == begin) {
            return syntax_error("a '{' quantifier needs a count", brace);
        }
        return {};
    }

    status parse_atom(std::size_t& out, bool& quantifiable) {
        switch (source_[pos_]) {
        case '(':
            return parse_group(out, quantifiable);
        case '[':
            return parse_class(out);
        case '.':
            ++pos_;
            out = add_plain(ast_node::kind::any);
            return {};
        case '^':
            ++pos_;
            quantifiable = false;
            out = add_plain(ast_node::kind::begin_line);
            return {};
        case '$':
            ++pos_;
            quantifiable = false;
            out = add_plain(ast_node::kind::end_line);
            return {};
        case '\\':
            return parse_escape(out, quantifiable);
        case '*':
        case '+':
        case '?':
        case '{':
            return syntax_error("nothing to repeat", pos_);
        default:
            ++pos_;
            out = add_byte(static_cast<unsigned char>(source_[pos_ - 1]));
            return {};
        }
    }

    status parse_group(std::size_t& out, bool& quantifiable) {
        const std::size_t open = pos_;
        ++pos_;  // '('
        bool capturing = true;
        if (peek() == '?') {
            ++pos_;
            if (peek() == ':') {
                ++pos_;
                capturing = false;
            } else {
                return syntax_error("unsupported '(?' group syntax", open);
            }
        }
        std::size_t group = 0;
        if (capturing) {
            if (group_count_ >= regex_max_groups) {
                return limit_error(
                    "the pattern has more capture groups than the limit");
            }
            group = ++group_count_;
            closed_.push_back(false);
        }
        if (depth_ + 1 > max_nesting) {
            return limit_error("the pattern nests groups too deeply");
        }
        ++depth_;
        std::size_t inner = 0;
        status parsed = parse_alternation(inner);
        --depth_;
        if (!parsed.ok()) {
            return parsed;
        }
        if (peek() != ')') {
            return syntax_error("unterminated group", open);
        }
        ++pos_;
        quantifiable = true;
        if (!capturing) {
            out = inner;
            return {};
        }
        closed_[group] = true;
        ast_node node;
        node.type = ast_node::kind::group;
        node.value = static_cast<std::uint32_t>(group);
        node.children.push_back(inner);
        out = add(std::move(node));
        return {};
    }

    status parse_escape(std::size_t& out, bool& quantifiable) {
        const std::size_t escape_at = pos_;
        ++pos_;
        if (pos_ >= source_.size()) {
            return syntax_error("trailing backslash", escape_at);
        }
        const char escaped = source_[pos_];
        if (escaped >= '1' && escaped <= '9') {
            const auto group = static_cast<std::uint32_t>(escaped - '0');
            ++pos_;
            if (group > group_count_ || !closed_[group]) {
                return syntax_error(
                    "back reference to a group that is not closed yet",
                    escape_at);
            }
            ast_node node;
            node.type = ast_node::kind::backref;
            node.value = group;
            out = add(std::move(node));
            return {};
        }
        if (escaped == 'b' || escaped == 'B') {
            ++pos_;
            quantifiable = false;
            out = add_plain(escaped == 'b' ? ast_node::kind::word_boundary
                                           : ast_node::kind::not_word_boundary);
            return {};
        }
        byte_set set;
        if (build_escape_set(escaped, set)) {
            ++pos_;
            out = add_class(std::move(set));
            return {};
        }
        unsigned char value = 0;
        if (!build_escape_byte(escaped, value, /*in_class=*/false)) {
            return syntax_error("unsupported escape", escape_at);
        }
        ++pos_;
        out = add_byte(value);
        return {};
    }

    status parse_class(std::size_t& out) {
        const std::size_t open = pos_;
        ++pos_;  // '['
        bool negate = false;
        if (peek() == '^') {
            negate = true;
            ++pos_;
        }
        byte_set set;
        bool any = false;
        for (;;) {
            if (pos_ >= source_.size()) {
                return syntax_error("unterminated character class", open);
            }
            if (source_[pos_] == ']' && any) {
                ++pos_;
                break;
            }
            const std::size_t member_at = pos_;
            std::uint32_t low = 0;
            if (source_[pos_] == '\\') {
                ++pos_;
                if (pos_ >= source_.size()) {
                    return syntax_error("trailing backslash", member_at);
                }
                const char escaped = source_[pos_];
                byte_set escaped_set;
                if (build_escape_set(escaped, escaped_set)) {
                    ++pos_;
                    set.unite(escaped_set);
                    any = true;
                    if (peek() == '-' && range_continues()) {
                        return syntax_error(
                            "a class escape cannot start a range", member_at);
                    }
                    continue;
                }
                unsigned char value = 0;
                if (!build_escape_byte(escaped, value, /*in_class=*/true)) {
                    return syntax_error("unsupported escape in a character "
                                        "class",
                                        member_at);
                }
                ++pos_;
                low = value;
            } else {
                low = static_cast<unsigned char>(source_[pos_]);
                ++pos_;
            }
            any = true;
            if (peek() == '-' && range_continues()) {
                ++pos_;  // '-'
                const std::size_t high_at = pos_;
                std::uint32_t high = 0;
                if (source_[pos_] == '\\') {
                    ++pos_;
                    if (pos_ >= source_.size()) {
                        return syntax_error("trailing backslash", high_at);
                    }
                    const char escaped = source_[pos_];
                    byte_set escaped_set;
                    if (build_escape_set(escaped, escaped_set)) {
                        return syntax_error(
                            "a class escape cannot end a range", high_at);
                    }
                    unsigned char value = 0;
                    if (!build_escape_byte(escaped, value, /*in_class=*/true)) {
                        return syntax_error(
                            "unsupported escape in a character class", high_at);
                    }
                    ++pos_;
                    high = value;
                } else {
                    high = static_cast<unsigned char>(source_[pos_]);
                    ++pos_;
                }
                if (high < low) {
                    return syntax_error("invalid character range", high_at);
                }
                for (std::uint32_t value = low; value <= high; ++value) {
                    set.add(fold_if(static_cast<unsigned char>(value),
                                    options_.icase));
                }
            } else {
                set.add(fold_if(static_cast<unsigned char>(low), options_.icase));
            }
        }
        if (negate) {
            set.invert();
        }
        out = add_class(std::move(set));
        return {};
    }

    // True when the '-' at the current position starts a range instead of
    // being a literal hyphen at the end of the class.
    bool range_continues() const noexcept {
        return pos_ + 1 < source_.size() && source_[pos_ + 1] != ']';
    }

    bool build_escape_set(char escape, byte_set& set) const {
        switch (escape) {
        case 'd':
        case 'D':
            add_predicate(set, is_digit_byte, escape == 'D');
            return true;
        case 'w':
        case 'W':
            add_predicate(set, is_word_byte, escape == 'W');
            return true;
        case 's':
        case 'S':
            add_predicate(set, is_space_byte, escape == 'S');
            return true;
        default:
            return false;
        }
    }

    template <typename Predicate>
    void add_predicate(byte_set& set, Predicate pred, bool negate) const {
        for (std::uint32_t value = 0; value < 256; ++value) {
            const auto byte = static_cast<unsigned char>(value);
            if (pred(byte) != negate) {
                set.add(fold_if(byte, options_.icase));
            }
        }
    }

    bool build_escape_byte(char escape, unsigned char& value,
                           bool in_class) const {
        switch (escape) {
        case 'n':
            value = '\n';
            return true;
        case 't':
            value = '\t';
            return true;
        case 'r':
            value = '\r';
            return true;
        case 'f':
            value = '\f';
            return true;
        case 'v':
            value = '\v';
            return true;
        case 'b':
            // Outside a class '\b' is the word boundary and is handled before
            // this point; inside one it is the backspace byte, as in Python.
            if (in_class) {
                value = '\b';
                return true;
            }
            return false;
        default:
            break;
        }
        if (is_ascii_alnum(escape)) {
            return false;
        }
        value = static_cast<unsigned char>(escape);
        return true;
    }

    char peek() const noexcept {
        return pos_ < source_.size() ? source_[pos_] : '\0';
    }

    std::size_t add(ast_node node) {
        tree_.nodes.push_back(std::move(node));
        return tree_.nodes.size() - 1;
    }

    std::size_t add_plain(ast_node::kind type) {
        ast_node node;
        node.type = type;
        return add(std::move(node));
    }

    std::size_t add_byte(unsigned char value) {
        ast_node node;
        node.type = ast_node::kind::byte;
        node.byte_value = value;
        return add(std::move(node));
    }

    std::size_t add_class(byte_set set) {
        ast_node node;
        node.type = ast_node::kind::char_class;
        node.value = static_cast<std::uint32_t>(tree_.classes.size());
        tree_.classes.push_back(set);
        return add(std::move(node));
    }

    std::string_view source_;
    const regex_options& options_;
    ast_tree& tree_;
    std::size_t pos_ = 0;
    std::size_t group_count_ = 0;
    std::size_t depth_ = 0;
    // closed_[group] tells whether that group's ')' has been seen, so a back
    // reference inside the very group it names is rejected.
    std::vector<bool> closed_{false};
};

// --- Emitter -----------------------------------------------------------------

// True when `index` can match without consuming input. A repetition whose body
// cannot match empty needs no progress check, which keeps the common `a*` at
// three steps per byte instead of four.
bool can_match_empty(const ast_tree& tree, std::size_t index) noexcept {
    const ast_node& node = tree.nodes[index];
    switch (node.type) {
    case ast_node::kind::empty:
    case ast_node::kind::begin_line:
    case ast_node::kind::end_line:
    case ast_node::kind::word_boundary:
    case ast_node::kind::not_word_boundary:
        return true;
    case ast_node::kind::byte:
    case ast_node::kind::any:
    case ast_node::kind::char_class:
        return false;
    case ast_node::kind::backref:
        // A group can capture the empty string, so a back reference might too.
        return true;
    case ast_node::kind::group:
        return can_match_empty(tree, node.children.front());
    case ast_node::kind::concat:
        for (const std::size_t child : node.children) {
            if (!can_match_empty(tree, child)) {
                return false;
            }
        }
        return true;
    case ast_node::kind::alternate:
        for (const std::size_t child : node.children) {
            if (can_match_empty(tree, child)) {
                return true;
            }
        }
        return false;
    case ast_node::kind::repeat:
        return node.min == 0 || can_match_empty(tree, node.children.front());
    }
    return true;
}

class program_emitter {
public:
    program_emitter(const ast_tree& tree, bool icase, std::size_t extra_base,
                    std::vector<instruction>& code)
        : tree_(tree), icase_(icase), extra_base_(extra_base), code_(code) {}

    bool emit(std::size_t index) {
        if (failed_) {
            return false;
        }
        const ast_node& node = tree_.nodes[index];
        switch (node.type) {
        case ast_node::kind::empty:
            return true;
        case ast_node::kind::byte:
            return push(opcode::byte,
                        fold_if(node.byte_value, icase_));
        case ast_node::kind::any:
            return push(opcode::any);
        case ast_node::kind::char_class:
            return push(opcode::char_class, node.value);
        case ast_node::kind::begin_line:
            return push(opcode::begin_line);
        case ast_node::kind::end_line:
            return push(opcode::end_line);
        case ast_node::kind::word_boundary:
            return push(opcode::word_boundary);
        case ast_node::kind::not_word_boundary:
            return push(opcode::not_word_boundary);
        case ast_node::kind::backref:
            return push(opcode::group_ref, node.value);
        case ast_node::kind::concat:
            for (const std::size_t child : node.children) {
                if (!emit(child)) {
                    return false;
                }
            }
            return true;
        case ast_node::kind::alternate:
            return emit_alternate(node);
        case ast_node::kind::repeat:
            return emit_repeat(node);
        case ast_node::kind::group:
            return emit_group(node);
        }
        return false;
    }

    bool failed() const noexcept { return failed_; }
    std::size_t extra_slots() const noexcept { return extra_slots_; }

private:
    bool push(opcode code, std::uint32_t a = 0, std::uint32_t b = 0,
              std::uint32_t c = 0) {
        if (code_.size() >= regex_max_program_size) {
            failed_ = true;
            return false;
        }
        code_.push_back(instruction{code, a, b, c});
        return true;
    }

    bool emit_group(const ast_node& node) {
        const auto slot = static_cast<std::uint32_t>(2 * node.value);
        if (!push(opcode::save, slot)) {
            return false;
        }
        if (!emit(node.children.front())) {
            return false;
        }
        return push(opcode::save, slot + 1);
    }

    bool emit_alternate(const ast_node& node) {
        std::vector<std::size_t> jumps;
        for (std::size_t i = 0; i + 1 < node.children.size(); ++i) {
            const std::size_t split = code_.size();
            if (!push(opcode::split)) {
                return false;
            }
            if (!emit(node.children[i])) {
                return false;
            }
            const std::size_t jump = code_.size();
            if (!push(opcode::jump)) {
                return false;
            }
            jumps.push_back(jump);
            code_[split].a = static_cast<std::uint32_t>(split + 1);
            code_[split].b = static_cast<std::uint32_t>(code_.size());
        }
        if (!emit(node.children.back())) {
            return false;
        }
        const auto end = static_cast<std::uint32_t>(code_.size());
        for (const std::size_t jump : jumps) {
            code_[jump].a = end;
        }
        return true;
    }

    // Bounded repeats compile to a counter loop instead of an expansion.
    // Expanding `X{2,3}` into copies is what makes nested repeats grow
    // multiplicatively (`(?:a{2,3}){2,3}` is nine copies, one more level is
    // twenty-seven, ...) until a legal pattern is rejected for size; a loop
    // keeps the program proportional to the pattern however the bounds nest.
    //
    // Shape (greedy; lazy swaps the split's two targets):
    //
    //     count_init  c
    //   head: split      body, check_exit
    //   body: count_ge_jump c, max, exit     ; at the limit, only exit is legal
    //         count_add  c
    //         <child>
    //         jump       head
    // check_exit: count_ge_jump c, min, exit ; fewer than `min` cannot exit
    //         fail
    //   exit:
    //
    // The split is the loop's backtrack point, so the engine can still try
    // fewer iterations exactly as it could with an expansion; only the code
    // size changed.
bool emit_repeat(const ast_node& node) {
        const std::size_t child = node.children.front();
        if (node.max == unbounded) {
            // `X*`, `X+`, `X{n,}`: `min` mandatory iterations in a counter
            // loop, then the unbounded loop. The mandatory part is exact, so a
            // lazy suffix does not make it lazy; laziness applies to the tail.
            if (node.min > 0 && !emit_counted(node, node.min)) {
                return false;
            }
            return emit_star(child, node.lazy,
                             can_match_empty(tree_, child));
        }
        return emit_counted(node, node.max);
    }

    // Emits `X{lower,upper}` greedily or lazily. The lower bound comes from
    // `node.min`; `upper_bound` is finite (the caller handles `{n,}`).
    //
    // Shape (greedy; lazy swaps the split's two targets):
    //
    //     count_init  c
    //   head: count_ge_jump c, upper, exit   ; at the limit, exit without a frame
    //         split        body, check_exit
    //   body: count_add    c
    //         mark          m                 ; only when the body can be empty
    //         <child>
    //         count_ge_jump c, lower, empty_chk
    //         jump         head
    //  empty_chk: progress   m, head          ; consumed nothing this iteration
    //         jump         exit               ; -> stop repeating
    // check_exit: count_ge_jump c, lower, exit
    //         fail                           ; fewer than `lower` cannot exit
    //   exit:
    //
    // The split is the loop's backtrack point, so the engine can still try
    // fewer iterations exactly as with an expansion; only the code size is
    // constant instead of proportional to the bounds.
    //
    // The empty-body check mirrors the reference semantics: once an iteration
    // matches nothing and the counter has reached the minimum, repeating
    // further cannot change the outcome, so the loop exits right there
    // instead of counting empty iterations up to `upper`.
    bool emit_counted(const ast_node& node, std::size_t upper_bound) {
        const std::size_t child = node.children.front();
        const auto counter =
            static_cast<std::uint32_t>(extra_base_ + extra_slots_++);
        const auto upper = static_cast<std::uint32_t>(upper_bound);
        const auto lower = static_cast<std::uint32_t>(node.min);
        if (!push(opcode::count_init, counter)) {
            return false;
        }
        const std::size_t head = code_.size();
        const std::size_t limit_guard = code_.size();
        if (!push(opcode::count_ge_jump, counter, upper)) {
            return false;
        }
        const std::size_t split_site = code_.size();
        if (!push(opcode::split)) {
            return false;
        }
        const std::size_t body = code_.size();
        if (!push(opcode::count_add, counter)) {
            return false;
        }
        const bool child_can_be_empty = can_match_empty(tree_, child);
        std::uint32_t iteration_mark = 0;
        std::size_t empty_check = 0;
        if (child_can_be_empty) {
            iteration_mark =
                static_cast<std::uint32_t>(extra_base_ + extra_slots_++);
            if (!push(opcode::mark, iteration_mark)) {
                return false;
            }
        }
        if (!emit(child)) {
            return false;
        }
        if (child_can_be_empty) {
            empty_check = code_.size();
            if (!push(opcode::count_ge_jump, counter, lower)) {
                return false;
            }
            if (!push(opcode::jump, static_cast<std::uint32_t>(head))) {
                return false;
            }
            if (!push(opcode::progress, iteration_mark,
                      static_cast<std::uint32_t>(head))) {
                return false;
            }
            if (!push(opcode::jump, 0)) {
                return false;
            }
            // Below `lower` the loop must keep iterating even on empty
            // matches; at or above it, an empty iteration exits.
            code_[empty_check].c =
                static_cast<std::uint32_t>(empty_check + 2);
        } else if (!push(opcode::jump, static_cast<std::uint32_t>(head))) {
            return false;
        }
        const std::size_t check_exit = code_.size();
        if (!push(opcode::count_ge_jump, counter, lower)) {
            return false;
        }
        if (!push(opcode::fail)) {
            return false;
        }
        const std::size_t exit = code_.size();
        code_[limit_guard].c = static_cast<std::uint32_t>(exit);
        code_[split_site].a = node.lazy ? static_cast<std::uint32_t>(check_exit)
                                        : static_cast<std::uint32_t>(body);
        code_[split_site].b = node.lazy ? static_cast<std::uint32_t>(body)
                                        : static_cast<std::uint32_t>(check_exit);
        code_[check_exit].c = static_cast<std::uint32_t>(exit);
        if (child_can_be_empty) {
            // The `jump exit` right after the empty-iteration check.
            code_[empty_check + 3].a = static_cast<std::uint32_t>(exit);
        }
        return true;
    }

    // `X*`. When the body can match empty a mark/progress pair stops the loop
    // as soon as an iteration consumes nothing; the captures of that empty
    // iteration are left in place, which is Python's behaviour. Otherwise a
    // plain jump back to the head closes the loop: the loop head's split is
    // what records the exit alternative for backtracking.
    bool emit_star(std::size_t child, bool lazy, bool body_can_be_empty) {
        std::size_t slot = 0;
        if (body_can_be_empty) {
            slot = extra_base_ + extra_slots_++;
        }
        const std::size_t head = code_.size();
        if (!push(opcode::split)) {
            return false;
        }
        const std::size_t body = code_.size();
        if (body_can_be_empty &&
            !push(opcode::mark, static_cast<std::uint32_t>(slot))) {
            return false;
        }
        if (!emit(child)) {
            return false;
        }
        if (body_can_be_empty) {
            if (!push(opcode::progress, static_cast<std::uint32_t>(slot),
                      static_cast<std::uint32_t>(head))) {
                return false;
            }
        } else if (!push(opcode::jump, static_cast<std::uint32_t>(head))) {
            return false;
        }
        const std::size_t exit = code_.size();
        code_[head].a = lazy ? static_cast<std::uint32_t>(exit)
                             : static_cast<std::uint32_t>(body);
        code_[head].b = lazy ? static_cast<std::uint32_t>(body)
                             : static_cast<std::uint32_t>(exit);
        return true;
    }

    const ast_tree& tree_;
    bool icase_;
    std::size_t extra_base_;
    std::vector<instruction>& code_;
    std::size_t extra_slots_ = 0;
    bool failed_ = false;
};

// --- Replacement template ----------------------------------------------------

struct replacement_part {
    bool is_group = false;
    std::size_t index = 0;
    std::string_view literal;
};

// Parses `$0`..`$9` and `\\`. The whole template is validated before any
// matching happens, so a bad template fails even when nothing would match.
result<std::vector<replacement_part>>
parse_replacement(std::string_view text, std::size_t group_count) {
    std::vector<replacement_part> parts;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\\') {
            if (i + 1 >= text.size() || text[i + 1] != '\\') {
                return rpc_error{rpc_code::invalid_argument,
                                 "regex: a backslash in the replacement must "
                                 "be doubled"};
            }
            replacement_part part;
            part.literal = text.substr(i, 1);
            parts.push_back(part);
            i += 2;
            continue;
        }
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] >= '0' &&
            text[i + 1] <= '9') {
            const auto group = static_cast<std::size_t>(text[i + 1] - '0');
            if (group > group_count) {
                return rpc_error{
                    rpc_code::invalid_argument,
                    "regex: the replacement refers to group " +
                        std::to_string(group) + " but the pattern has " +
                        std::to_string(group_count) + " capture group(s)"};
            }
            replacement_part part;
            part.is_group = true;
            part.index = group;
            parts.push_back(part);
            i += 2;
            continue;
        }
        std::size_t end = i + 1;
        while (end < text.size() && text[end] != '\\' &&
               !(text[end] == '$' && end + 1 < text.size() &&
                 text[end + 1] >= '0' && text[end + 1] <= '9')) {
            ++end;
        }
        replacement_part part;
        part.literal = text.substr(i, end - i);
        parts.push_back(part);
        i = end;
    }
    return parts;
}

void append_expansion(std::string& out, std::string_view text,
                      const regex_match& match,
                      const std::vector<replacement_part>& parts) {
    for (const replacement_part& part : parts) {
        if (!part.is_group) {
            out.append(part.literal);
            continue;
        }
        if (part.index == 0) {
            out.append(text.substr(match.begin, match.end - match.begin));
            continue;
        }
        const std::pair<std::size_t, std::size_t>& span =
            match.groups[part.index - 1];
        if (span.first == regex_match::npos) {
            continue;
        }
        out.append(text.substr(span.first, span.second - span.first));
    }
}

} // namespace

// --- Compiled program --------------------------------------------------------

struct regex::impl {
    std::string source;
    regex_options options;
    std::vector<instruction> program;
    std::vector<byte_set> classes;
    std::size_t group_count = 0;
    std::size_t slot_count = 0;

    struct frame {
        std::size_t pc;
        std::size_t pos;
        std::size_t trail;
    };

    struct undo_entry {
        std::size_t slot;
        std::size_t old_value;
    };

    struct state {
        std::vector<std::size_t> slots;
        std::vector<frame> stack;
        std::vector<undo_entry> trail;
        std::size_t steps = 0;
    };

    enum class outcome { matched, no_match, exhausted };

    // Runs one attempt anchored at `start`. `st` is reused across starting
    // positions so the vectors are allocated once per public call; the step
    // counter is reset here, so the budget bounds one attempt (one starting
    // position), not the whole scan. Bounding the attempt is what catches a
    // catastrophic pattern: it explodes inside a single starting position.
    outcome run(std::string_view text, std::size_t start, state& st) const {
        const std::size_t size = text.size();
        const bool icase = options.icase;
        const bool dotall = options.dotall;
        const bool multiline = options.multiline;
        const std::size_t budget = options.max_steps;

        st.slots.assign(slot_count, regex_match::npos);
        st.stack.clear();
        st.trail.clear();
        st.steps = 0;

        std::size_t pc = 0;
        std::size_t pos = start;

        for (;;) {
            // The budget is checked before every step, so a run stops with
            // exactly `budget` executed steps and never one more.
            if (st.steps >= budget) {
                return outcome::exhausted;
            }
            ++st.steps;

            const instruction in = program[pc];
            bool ok = true;

            switch (in.code) {
            case opcode::byte:
                if (pos < size &&
                    fold_if(static_cast<unsigned char>(text[pos]), icase) ==
                        static_cast<unsigned char>(in.a)) {
                    ++pos;
                    ++pc;
                } else {
                    ok = false;
                }
                break;
            case opcode::any:
                if (pos < size && (dotall || text[pos] != '\n')) {
                    ++pos;
                    ++pc;
                } else {
                    ok = false;
                }
                break;
            case opcode::char_class:
                if (pos < size &&
                    classes[in.a].contains(
                        fold_if(static_cast<unsigned char>(text[pos]), icase))) {
                    ++pos;
                    ++pc;
                } else {
                    ok = false;
                }
                break;
            case opcode::split:
                st.stack.push_back(frame{in.b, pos, st.trail.size()});
                pc = in.a;
                break;
            case opcode::jump:
                pc = in.a;
                break;
case opcode::save:
            case opcode::mark:
                if (!st.stack.empty()) {
                    st.trail.push_back(undo_entry{in.a, st.slots[in.a]});
                }
                st.slots[in.a] = pos;
                ++pc;
                break;
            case opcode::count_init:
                if (!st.stack.empty()) {
                    st.trail.push_back(undo_entry{in.a, st.slots[in.a]});
                }
                st.slots[in.a] = 0;
                ++pc;
                break;
            case opcode::count_add:
                if (!st.stack.empty()) {
                    st.trail.push_back(undo_entry{in.a, st.slots[in.a]});
                }
                ++st.slots[in.a];
                ++pc;
                break;
            case opcode::count_ge_jump:
                pc = (st.slots[in.a] >= in.b) ? in.c : pc + 1;
                break;
            case opcode::fail:
                ok = false;
                break;
            case opcode::progress:
                // Not `!=`: an iteration that consumed nothing must exit the
                // loop, or an empty body would spin forever.
                if (pos > st.slots[in.a]) {
                    pc = in.b;
                } else {
                    ++pc;
                }
                break;
            case opcode::begin_line:
                if (pos == 0 || (multiline && text[pos - 1] == '\n')) {
                    ++pc;
                } else {
                    ok = false;
                }
                break;
            case opcode::end_line:
                if (pos == size ||
                    (text[pos] == '\n' && (multiline || pos + 1 == size))) {
                    ++pc;
                } else {
                    ok = false;
                }
                break;
            case opcode::word_boundary:
            case opcode::not_word_boundary: {
                const bool before =
                    pos > 0 &&
                    is_word_byte(static_cast<unsigned char>(text[pos - 1]));
                const bool after =
                    pos < size &&
                    is_word_byte(static_cast<unsigned char>(text[pos]));
                const bool boundary = before != after;
                if (boundary == (in.code == opcode::word_boundary)) {
                    ++pc;
                } else {
                    ok = false;
                }
                break;
            }
            case opcode::group_ref: {
                const std::size_t begin = st.slots[2 * in.a];
                const std::size_t end = st.slots[2 * in.a + 1];
                if (begin == regex_match::npos || end == regex_match::npos ||
                    end - begin > size - pos) {
                    ok = false;
                    break;
                }
                for (std::size_t k = 0; k < end - begin; ++k) {
                    const unsigned char captured = fold_if(
                        static_cast<unsigned char>(text[begin + k]), icase);
                    const unsigned char subject = fold_if(
                        static_cast<unsigned char>(text[pos + k]), icase);
                    if (captured != subject) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    pos += end - begin;
                    ++pc;
                }
                break;
            }
            case opcode::accept:
                return outcome::matched;
            }

            if (!ok) {
                if (st.stack.empty()) {
                    return outcome::no_match;
                }
                const frame top = st.stack.back();
                st.stack.pop_back();
                while (st.trail.size() > top.trail) {
                    const undo_entry entry = st.trail.back();
                    st.trail.pop_back();
                    st.slots[entry.slot] = entry.old_value;
                }
                pc = top.pc;
                pos = top.pos;
            }
        }
    }

    regex_match make_match(const state& st) const {
        regex_match out;
        out.matched = true;
        out.begin = st.slots[0];
        out.end = st.slots[1];
        out.groups.resize(group_count);
        for (std::size_t group = 1; group <= group_count; ++group) {
            out.groups[group - 1] = {st.slots[2 * group], st.slots[2 * group + 1]};
        }
        return out;
    }

    rpc_error step_budget_error(std::size_t steps) const {
        return rpc_error{
            rpc_code::resource_exhausted,
            "regex: backtracking step budget exhausted (steps=" +
                std::to_string(steps) + ", max_steps=" +
                std::to_string(options.max_steps) + ")"};
    }

    result<regex_match> search_from(std::string_view text,
                                    std::size_t from) const {
        state st;
        st.steps = 0;
        for (std::size_t start = from; start <= text.size(); ++start) {
            const outcome attempt = run(text, start, st);
            if (attempt == outcome::matched) {
                return make_match(st);
            }
            if (attempt == outcome::exhausted) {
                return step_budget_error(st.steps);
            }
        }
        return regex_match{};
    }

    result<std::vector<regex_match>> find_all_in(std::string_view text) const {
        std::vector<regex_match> out;
        state st;
        st.steps = 0;
        std::size_t from = 0;
        for (;;) {
            bool found = false;
            for (std::size_t start = from; start <= text.size(); ++start) {
                const outcome attempt = run(text, start, st);
                if (attempt == outcome::exhausted) {
                    return step_budget_error(st.steps);
                }
                if (attempt == outcome::matched) {
                    out.push_back(make_match(st));
                    found = true;
                    break;
                }
            }
            if (!found) {
                break;
            }
            const regex_match& last = out.back();
            if (last.end > last.begin) {
                from = last.end;
            } else if (last.end < text.size()) {
                // An empty match advances by one byte; otherwise a zero-width
                // pattern at a fixed position would be found forever.
                from = last.end + 1;
            } else {
                break;
            }
        }
        return out;
    }
};

// --- Public interface --------------------------------------------------------

regex::regex(std::shared_ptr<const impl> compiled) noexcept
    : impl_(std::move(compiled)) {}

result<regex> regex::compile(std::string_view pattern, regex_options options) {
    if (pattern.size() > regex_max_pattern_length) {
        return limit_error("the pattern is longer than the length limit");
    }

    ast_tree tree;
    tree.nodes.push_back(ast_node{});  // index 0: the empty node

    pattern_parser parser(pattern, options, tree);
    std::size_t root = 0;
    const status parsed = parser.parse(root);
    if (!parsed.ok()) {
        return parsed.error();
    }

    std::vector<instruction> program;
    program.push_back(instruction{opcode::save, 0, 0, 0});
    // Mark slots live after the capture slots: slot 0/1 hold the whole match
    // and each capture group owns two more, so a mark can never alias one.
    const std::size_t capture_slots = 2 * (parser.group_count() + 1);
    program_emitter emitter(tree, options.icase, capture_slots, program);
    const bool emitted = emitter.emit(root);
    if (!emitted || emitter.failed()) {
        return limit_error("the compiled program is larger than the limit");
    }
    program.push_back(instruction{opcode::save, 1, 0, 0});
    program.push_back(instruction{opcode::accept, 0, 0, 0});
    if (program.size() > regex_max_program_size) {
        return limit_error("the compiled program is larger than the limit");
    }

    auto compiled = std::make_shared<impl>();
    compiled->source = std::string(pattern);
    compiled->options = options;
    compiled->program = std::move(program);
    compiled->classes = std::move(tree.classes);
    compiled->group_count = parser.group_count();
    // Slots 0/1 are the whole match, then two per capture group, then one per
    // repetition that needed an empty-progress mark.
    compiled->slot_count = capture_slots + emitter.extra_slots();

    return regex(std::move(compiled));
}

bool regex::is_match(std::string_view text) const {
    const result<regex_match> found = search(text);
    return found.ok() && found.value().matched;
}

result<regex_match> regex::search(std::string_view text) const {
    if (!impl_) {
        return rpc_error{rpc_code::failed_precondition,
                         "regex: the object has no compiled pattern"};
    }
    return impl_->search_from(text, 0);
}

result<std::vector<regex_match>>
regex::find_all(std::string_view text) const {
    if (!impl_) {
        return rpc_error{rpc_code::failed_precondition,
                         "regex: the object has no compiled pattern"};
    }
    return impl_->find_all_in(text);
}

result<std::string> regex::replace_first(std::string_view text,
                                         std::string_view replacement) const {
    if (!impl_) {
        return rpc_error{rpc_code::failed_precondition,
                         "regex: the object has no compiled pattern"};
    }
    const result<std::vector<replacement_part>> parts =
        parse_replacement(replacement, impl_->group_count);
    if (!parts.ok()) {
        return parts.error();
    }
    const result<regex_match> found = impl_->search_from(text, 0);
    if (!found.ok()) {
        return found.error();
    }
    if (!found.value().matched) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size() + replacement.size());
    out.append(text.substr(0, found.value().begin));
    append_expansion(out, text, found.value(), parts.value());
    out.append(text.substr(found.value().end));
    return out;
}

result<std::string> regex::replace_all(std::string_view text,
                                       std::string_view replacement) const {
    if (!impl_) {
        return rpc_error{rpc_code::failed_precondition,
                         "regex: the object has no compiled pattern"};
    }
    const result<std::vector<replacement_part>> parts =
        parse_replacement(replacement, impl_->group_count);
    if (!parts.ok()) {
        return parts.error();
    }
    const result<std::vector<regex_match>> matches = impl_->find_all_in(text);
    if (!matches.ok()) {
        return matches.error();
    }
    std::string out;
    out.reserve(text.size() + replacement.size() * matches.value().size());
    std::size_t cursor = 0;
    for (const regex_match& match : matches.value()) {
        out.append(text.substr(cursor, match.begin - cursor));
        append_expansion(out, text, match, parts.value());
        cursor = match.end;
    }
    out.append(text.substr(cursor));
    return out;
}

std::string_view regex::pattern() const noexcept {
    return impl_ ? std::string_view(impl_->source) : std::string_view();
}

} // namespace text
} // namespace crpc

