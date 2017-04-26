/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "LuaHandler.hxx"
#include "lb_config.hxx"
#include "http_server/Request.hxx"
#include "http_response.hxx"
#include "pool.hxx"
#include "GException.hxx"
#include "lua/RunFile.hxx"
#include "lua/Util.hxx"
#include "lua/Class.hxx"
#include "lua/Error.hxx"
#include "util/RuntimeError.hxx"
#include "util/ScopeExit.hxx"

#include <daemon/log.h>

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}

struct LbLuaRequestData {
    HttpServerRequest &request;
    HttpResponseHandler &handler;
    bool stale = false;

    explicit LbLuaRequestData(HttpServerRequest &_request,
                              HttpResponseHandler &_handler)
        :request(_request), handler(_handler) {}
};

static constexpr char lua_request_class[] = "lb.http_request";
typedef Lua::Class<LbLuaRequestData, lua_request_class> LbLuaRequest;

static LbLuaRequestData *
NewLuaRequest(lua_State *L, HttpServerRequest &request,
              HttpResponseHandler &handler)
{
    return LbLuaRequest::New(L, request, handler);
}

static LbLuaRequestData *
CheckLuaRequestData(lua_State *L, int idx)
{
    auto *data = LbLuaRequest::Check(L, idx);
    if (data->stale)
        luaL_error(L, "Stale request");

    return data;
}

static int
GetHeader(lua_State *L)
{
    if (lua_gettop(L) != 2)
        return luaL_error(L, "Invalid parameters");

    auto &data = *(LbLuaRequestData *)CheckLuaRequestData(L, 1);

    if (!lua_isstring(L, 2))
        return luaL_argerror(L, 2, "String expected");

    const char *name = lua_tostring(L, 2);

    const char *value = data.request.headers.Get(name);
    if (value != nullptr) {
        Lua::Push(L, value);
        return 1;
    } else
        return 0;
}

static int
SendMessage(lua_State *L)
{
    const unsigned top = lua_gettop(L);
    if (top < 2 || top > 3)
        return luaL_error(L, "Invalid parameters");

    auto &data = *(LbLuaRequestData *)CheckLuaRequestData(L, 1);

    http_status_t status = HTTP_STATUS_OK;
    const char *msg;

    unsigned i = 2;

    if (top > 2) {
        if (!lua_isnumber(L, i))
            return luaL_argerror(L, i, "Integer status expected");

        status = http_status_t(lua_tointeger(L, i));
        if (!http_status_is_valid(status))
            return luaL_argerror(L, i, "Invalid HTTP status");

        ++i;
    }

    if (!lua_isstring(L, i))
        return luaL_argerror(L, i, "Message expected");

    msg = lua_tostring(L, i);

    if (http_status_is_empty(status))
        msg = nullptr;

    data.stale = true;
    data.handler.InvokeResponse(data.request.pool, status,
                                p_strdup(data.request.pool, msg));
    return 0;
}

static constexpr struct luaL_reg request_methods [] = {
    {"get_header", GetHeader},
    {"send_message", SendMessage},
    {nullptr, nullptr}
};

static int
LbLuaRequestIndex(lua_State *L)
{
    if (lua_gettop(L) != 2)
        return luaL_error(L, "Invalid parameters");

    auto &data = *(LbLuaRequestData *)CheckLuaRequestData(L, 1);

    if (!lua_isstring(L, 2))
        luaL_argerror(L, 2, "string expected");

    const char *name = lua_tostring(L, 2);

    for (const auto *i = request_methods; i->name != nullptr; ++i) {
        if (strcmp(i->name, name) == 0) {
            Lua::Push(L, i->func);
            return 1;
        }
    }

    if (strcmp(name, "uri") == 0) {
        Lua::Push(L, data.request.uri);
        return 1;
    } else if (strcmp(name, "method") == 0) {
        Lua::Push(L, http_method_to_string(data.request.method));
        return 1;
    } else if (strcmp(name, "has_body") == 0) {
        Lua::Push(L, data.request.HasBody());
        return 1;
    } else if (strcmp(name, "remote_host") == 0) {
        Lua::Push(L, data.request.remote_host);
        return 1;
    }

    return luaL_error(L, "Unknown attribute");
}

LbLuaHandler::LbLuaHandler(const LbLuaHandlerConfig &config)
    :state(luaL_newstate()), function(state.get())
{
    auto *L = state.get();
    luaL_openlibs(L);

    Lua::RunFile(L, config.path.c_str());

    lua_getglobal(L, config.function.c_str());
    AtScopeExit(L) { lua_pop(L, 1); };

    if (!lua_isfunction(L, -1)) {
        if (lua_isnil(L, -1))
            throw FormatRuntimeError("No such function: '%s' in %s",
                                     config.function.c_str(),
                                     config.path.c_str());
        else
            throw FormatRuntimeError("Not a function: '%s' in %s",
                                     config.function.c_str(),
                                     config.path.c_str());
    }

    function.Set(Lua::StackIndex(-2));

    LbLuaRequest::Register(L);
    Lua::SetTable(L, -3, "__index", LbLuaRequestIndex);
    lua_pop(L, 1);
}

LbLuaHandler::~LbLuaHandler()
{
}

void
LbLuaHandler::HandleRequest(HttpServerRequest &request,
                            HttpResponseHandler &handler)
{
    auto *L = state.get();

    function.Push();
    auto *data = NewLuaRequest(L, request, handler);
    AtScopeExit(data) { data->stale = true; };

    if (lua_pcall(L, 1, 0, 0)) {
        auto error = Lua::PopError(L);
        if (!data->stale)
            handler.InvokeError(ToGError(error));
        return;
    }
}

void
LbLuaHandlerMap::Scan(const LbConfig &config)
{
    for (const auto &i : config.listeners)
        Scan(i);
}

void
LbLuaHandlerMap::Scan(const LbGotoIfConfig &config)
{
    Scan(config.destination);
}

void
LbLuaHandlerMap::Scan(const LbBranchConfig &config)
{
    Scan(config.fallback);

    for (const auto &i : config.conditions)
        Scan(i);
}

void
LbLuaHandlerMap::Scan(const LbGoto &g)
{
    if (g.lua != nullptr)
        Scan(*g.lua);

    if (g.branch != nullptr)
        Scan(*g.branch);
}

void
LbLuaHandlerMap::Scan(const LbListenerConfig &config)
{
    Scan(config.destination);
}

void
LbLuaHandlerMap::Scan(const LbLuaHandlerConfig &config)
{
    auto i = handlers.find(config.name);
    if (i != handlers.end())
        /* already added */
        return;

    handlers.emplace(std::piecewise_construct,
                     std::forward_as_tuple(config.name),
                     std::forward_as_tuple(config));
}
