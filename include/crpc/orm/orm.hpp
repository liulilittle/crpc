// CRPC -- ORM umbrella header. Everything ORM requires reflection, so without
// CRPC_HAVE_RTTI this header is empty and no ORM code compiles.
#ifndef CRPC_ORM_ORM_HPP
#define CRPC_ORM_ORM_HPP

#include <crpc/config.hpp>

#if defined(CRPC_HAVE_RTTI)

#include <crpc/orm/fwd.hpp>
#include <crpc/orm/annotate.hpp>
#include <crpc/orm/value.hpp>
#include <crpc/orm/metadata.hpp>
#include <crpc/orm/sql.hpp>
#include <crpc/orm/session.hpp>

#endif // CRPC_HAVE_RTTI

#endif // CRPC_ORM_ORM_HPP