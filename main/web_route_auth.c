#include "web_route_auth.h"

bool route_auth_allows(route_auth_class_t cls, bool setup_mode, bool authed)
{
    switch (cls) {
        case ROUTE_PUBLIC:
            return true;
        case ROUTE_SETUP_EXEMPT:
            return setup_mode || authed;
        case ROUTE_AUTH_REQUIRED:
        default:
            /* Fail closed: any unrecognized classification requires auth. */
            return authed;
    }
}
