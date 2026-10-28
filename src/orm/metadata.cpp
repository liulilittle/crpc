// CRPC -- entity metadata construction and validation.
#include <crpc/orm/metadata.hpp>

#include <string>

namespace crpc {
namespace orm {
namespace {

[[nodiscard]] bool named(const rtti::attr& attr, std::string_view name) {
    return attr.name != nullptr && name == attr.name;
}

[[nodiscard]] bool attr_flag(const rtti::attr& attr, std::string_view name) {
    // A flag is true when the bare attribute is present, or its first argument
    // is a non-zero integer / true boolean.
    return named(attr, name) &&
           (attr.args.empty() || attr.args[0].as_integer() != 0 || attr.args[0].as_bool());
}

[[nodiscard]] std::uint32_t attr_uint(const rtti::attr& attr, std::string_view name,
                                      std::uint32_t fallback) {
    if (!named(attr, name) || attr.args.empty()) {
        return fallback;
    }
    return static_cast<std::uint32_t>(attr.args[0].as_integer());
}

} // namespace

void apply_column_attributes(column_meta& column, span<const rtti::attr> attributes) {
    if (attributes.data() == nullptr) {
        return;
    }
    for (const rtti::attr& attr : attributes) {
        if (!named(attr, "column")) {
            continue;
        }
        if (!attr.args.empty()) {
            column.column.assign(attr.args[0].as_string().data(),
                                 attr.args[0].as_string().size());
        }
        break;
    }
    for (const rtti::attr& attr : attributes) {
        if (attr_flag(attr, "pk")) {
            column.primary_key = true;
        } else if (attr_flag(attr, "auto_increment")) {
            column.auto_increment = true;
        } else if (named(attr, "nullable") && !attr.args.empty()) {
            column.nullable = attr.args[0].as_integer() != 0;
        } else if (attr.name != nullptr && attr.name == std::string_view("length")) {
            column.length = attr_uint(attr, "length", 0);
        } else if (attr.name != nullptr && attr.name == std::string_view("precision")) {
            column.precision = attr_uint(attr, "precision", 0);
        } else if (attr.name != nullptr && attr.name == std::string_view("scale")) {
            column.scale = attr_uint(attr, "scale", 0);
        }
        // Unknown attribute names are ignored so future annotations stay
        // forward-compatible.
    }
}

void finalize_entity_metadata(entity_meta& meta) {
    meta.primary_key.clear();
    meta.auto_increment_index = orm_npos;

    for (const column_meta& column : meta.columns) {
        if (column.primary_key) {
            meta.primary_key.push_back(column.index);
        }
        if (column.auto_increment) {
            if (meta.auto_increment_index != orm_npos) {
                meta.valid = false;
                meta.error = "more than one auto-increment column is not supported";
                return;
            }
            meta.auto_increment_index = column.index;
        }
    }

    if (meta.table.empty()) {
        meta.valid = false;
        meta.error =
            "entity has no table name; declare it with CRPC_ORM_ENTITY(T, \"table\", ...)";
        return;
    }
    if (meta.columns.empty()) {
        meta.valid = false;
        meta.error = "entity has no reflected columns";
        return;
    }
    for (const column_meta& column : meta.columns) {
        if (column.column.empty()) {
            meta.valid = false;
            meta.error = "entity declares an empty column name";
            return;
        }
        if (column.auto_increment && !column.primary_key) {
            meta.valid = false;
            meta.error = "auto-increment column '" + column.column +
                         "' must also be marked as a primary key";
            return;
        }
    }
}

} // namespace orm
} // namespace crpc