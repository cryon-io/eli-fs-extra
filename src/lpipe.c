#include "lua.h"
#include "lauxlib.h"
#include "lutil.h"
#include "luaconf.h"
#include "lpipe.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <errno.h>

#ifndef _WIN32
static int closeonexec(int d)
{
    int fl = fcntl(d, F_GETFD);
    if (fl != -1)
        fl = fcntl(d, F_SETFD, fl | FD_CLOEXEC);
    return fl;
}
#endif

int pipe_is_nonblocking(lua_State *L)
{
    ELI_PIPE *_pipe = ((ELI_PIPE *)luaL_checkudata(L, 1, PIPE_METATABLE));
#ifdef _WIN32
    HANDLE h = _pipe->h;
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return push_error(L, "Failed nonblocking check");
    }
    if (GetFileType(h) == FILE_TYPE_PIPE)
    {
        DWORD state;
        if (!GetNamedPipeHandleState(h, &state, NULL, NULL, NULL, NULL, 0))
            return push_error(L, "Failed nonblocking check");

        lua_pushboolean(L, (state & PIPE_NOWAIT) != 0);
        _pipe->nonblocking = (flags & O_NONBLOCK) != 0;
    }
    else
    {
        lua_pushboolean(L, 0);
    }
#else
    int fd = _pipe->fd;
    if (fd < 0)
    {
        return push_error(L, "Failed nonblocking check");
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return push_error(L, "Failed to get file flags");
    }
    lua_pushboolean(L, (flags & O_NONBLOCK) != 0);
    _pipe->nonblocking = (flags & O_NONBLOCK) != 0;
#endif
    return 1;
}

int pipe_set_nonblocking(lua_State *L)
{
    ELI_PIPE *_pipe = ((ELI_PIPE *)luaL_checkudata(L, 1, PIPE_METATABLE));
    int nonblocking = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : 1;
#ifdef _WIN32
    HANDLE h = _pipe->h;
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return push_error(L, "Failed set nonblocking");
    }
    if (GetFileType(h) == FILE_TYPE_PIPE)
    {
        DWORD state;
        if (GetNamedPipeHandleState(h, &state, NULL, NULL, NULL, NULL, 0))
        {
            if (((state & PIPE_NOWAIT) != 0) == nonblocking)
            {
                lua_pushboolean(L, 1);
                _pipe->nonblocking = nonblocking;
                return 1;
            }

            if (nonblocking)
                state &= ~PIPE_NOWAIT;
            else
                state |= PIPE_NOWAIT;
            if (SetNamedPipeHandleState(h, &state, NULL, NULL))
            {
                lua_pushboolean(L, 1);
                _pipe->nonblocking = nonblocking;
                return 1;
            }
            errno = EINVAL;
            return push_error(L, "Failed set nonblocking");
        }
    }
    errno = ENOTSUP;
    return push_error(L, "Failed set nonblocking");
#else
    int fd = _pipe->fd;
    if (fd < 0)
    {
        return push_error(L, "Failed set nonblocking");
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return push_error(L, "Failed to get file flags");
    }
    if (((flags & O_NONBLOCK) != 0) == nonblocking)
    {
        lua_pushboolean(L, 1);
        _pipe->nonblocking = nonblocking;
        return 1;
    }
    if (nonblocking)
    {
        flags |= O_NONBLOCK;
    }
    else
    {
        flags &= ~O_NONBLOCK;
    }
    int res = fcntl(fd, F_SETFL, flags);
    if (res == 0)
    {
        _pipe->nonblocking = nonblocking;
    }
    return push_result(L, res, NULL);
#endif
}

// TODO: Implement win32 apis

static int pipe_close(lua_State *L)
{
    ELI_PIPE *p = ((ELI_PIPE *)luaL_checkudata(L, 1, PIPE_METATABLE));
    int res = close(p->fd);
    p->closed = 1;
    return luaL_fileresult(L, (res == 0), NULL);
}

static ELI_PIPE *new_pipe(lua_State *L, int fd, const char *mode)
{
    ELI_PIPE *p = (ELI_PIPE *)lua_newuserdata(L, sizeof(ELI_PIPE));
    luaL_getmetatable(L, PIPE_METATABLE);
    lua_setmetatable(L, -2);
    p->fd = fd;
    p->nonblocking = 0;
    p->closed = 0;
    return p;
}

/* -- in out/nil error */
int eli_pipe(lua_State *L)
{
#ifdef _WIN32
    HANDLE ph[2];
    if (!CreatePipe(ph + 0, ph + 1, 0, 0))
        return push_error(L, NULL);
    SetHandleInformation(ph[0], HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ph[1], HANDLE_FLAG_INHERIT, 0);
    new_pipe(L, ph[0], _O_RDONLY, "r");
    new_pipe(L, ph[1], _O_WRONLY, "w");
#else
    int fd[2];
    if (-1 == pipe(fd))
        return push_error(L, NULL);
    closeonexec(fd[0]);
    closeonexec(fd[1]);
    new_pipe(L, fd[0], "r");
    new_pipe(L, fd[1], "w");
#endif
    return 2;
}

static int pipe_write(lua_State *L)
{
    ELI_PIPE *_pipe = ((ELI_PIPE *)luaL_checkudata(L, 1, PIPE_METATABLE));
    lua_pushvalue(L, 1); /* push pipe at the stack top (to be returned) */
    int arg = 2;

    int nargs = lua_gettop(L) - arg;
    size_t status = 1;
    for (; status && nargs--; arg++)
    {
        size_t msgsize;
        const char *msg = luaL_checklstring(L, arg, &msgsize);
        status = status && (write(_pipe->fd, msg, msgsize) == msgsize);
    }
    if (status)
        return 1; /* file handle already on stack top */
    else
        return luaL_fileresult(L, status, NULL);
}

static int read_all(lua_State *L, ELI_PIPE *_pipe)
{
    size_t res;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    int fd = _pipe->fd;

    do
    { /* read file in chunks of LUAL_BUFFERSIZE bytes */
        char *p = luaL_prepbuffer(&b);
        res = read(fd, p, LUAL_BUFFERSIZE);
        if (res != -1)
            luaL_addlstring(&b, p, res);
    } while (res == LUAL_BUFFERSIZE);
    luaL_pushresult(&b); /* close buffer */
    if (res == -1)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK || !_pipe->nonblocking)
        {
            if (lua_rawlen(L, -1) == 0)
            {
                lua_pushnil(L);
            }
            lua_pushstring(L, strerror(errno));
            lua_pushinteger(L, errno);
            return 3;
        }
    }
    return 1;
}

static int read_line(lua_State *L, ELI_PIPE *_pipe, int chop)
{
    luaL_Buffer b;
    int fd = _pipe->fd;
    char c = '\0';
    luaL_buffinit(L, &b);
    size_t res = 1;

    while (res == 1 && c != EOF && c != '\n')
    {
        char *buff = luaL_prepbuffer(&b);
        int i = 0;
        while (i < LUAL_BUFFERSIZE && (res = read(fd, &c, sizeof(char))) == 1 && c != EOF && c != '\n')
        {
            buff[i++] = c;
        }

        if (res == -1)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK || !_pipe->nonblocking)
            {
                return push_error(L, NULL);
            }
        }
        luaL_addsize(&b, i);
    }
    if (!chop && c == '\n')
        luaL_addchar(&b, c);
    luaL_pushresult(&b);
    return 1; //(c == '\n' || lua_rawlen(L, -1) > 0);
}

static int pipe_read(lua_State *L)
{
    ELI_PIPE *_pipe = ((ELI_PIPE *)luaL_checkudata(L, 1, PIPE_METATABLE));

    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        size_t l = (size_t)luaL_checkinteger(L, 2);
        char *buffer = malloc(sizeof(char) * l);
        size_t res = read(_pipe->fd, buffer, l);
        if (res != -1)
        {
            lua_pushlstring(L, buffer, res);
            free(buffer);
            return 1;
        }
        free(buffer);
        return luaL_fileresult(L, res, NULL);
    }
    else
    {
        const char *p = luaL_checkstring(L, 2);
        size_t success;
        if (*p == '*')
            p++; /* skip optional '*' (for compatibility) */
        switch (*p)
        {
        case 'l': /* line */
            return read_line(L, _pipe, 1);
        case 'L': /* line with end-of-line */
            return read_line(L, _pipe, 0);
        case 'a':                      
            return read_all(L, _pipe); /* read all data available */
        default:
            return luaL_argerror(L, 2, "invalid format");
        }
    }
}

int pipe_create_meta(lua_State *L)
{
    luaL_newmetatable(L, PIPE_METATABLE);

    /* Method table */
    lua_newtable(L);
    lua_pushcfunction(L, pipe_read);
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, pipe_write);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, pipe_close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, pipe_set_nonblocking);
    lua_setfield(L, -2, "set_nonblocking");
    lua_pushcfunction(L, pipe_is_nonblocking);
    lua_setfield(L, -2, "is_nonblocking");

    /* Metamethods */
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, pipe_close);
    lua_setfield(L, -2, "__gc");
    return 1;
}
