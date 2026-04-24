/*
 * Emulator initialisation code
 *
 * Copyright 2000 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <limits.h>
#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif

#include "main.h"

static const char per_process_env_dir_var[] = "WINE_PROCESS_ENV_DIR";
static const char per_process_env_bases_var[] = "WINE_PROCESS_ENV_BASES";

#if defined(__APPLE__) && defined(__x86_64__) && !defined(HAVE_WINE_PRELOADER)

/* Not using the preloader on x86_64:
 * Reserve the same areas as the preloader does, but using zero-fill sections
 * (the only way to prevent system frameworks from using them, including allocations
 * before main() runs).
 */
__asm__(".zerofill WINE_RESERVE,WINE_RESERVE");
static char __wine_reserve[0x1fffff000] __attribute__((section("WINE_RESERVE, WINE_RESERVE")));

__asm__(".zerofill WINE_TOP_DOWN,WINE_TOP_DOWN");
static char __wine_top_down[0x001ff0000] __attribute__((section("WINE_TOP_DOWN, WINE_TOP_DOWN")));

static const struct wine_preload_info preload_info[] =
{
    { __wine_reserve,  sizeof(__wine_reserve)  }, /*         0x1000 -    0x200000000: low 8GB */
    { __wine_top_down, sizeof(__wine_top_down) }, /* 0x7ff000000000 - 0x7ff001ff0000: top-down allocations + virtual heap */
    { 0, 0 }                                      /* end of list */
};

const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = preload_info;

static void init_reserved_areas(void)
{
    int i;

    for (i = 0; wine_main_preload_info[i].size != 0; i++)
    {
        /* Match how the preloader maps reserved areas: */
        mmap(wine_main_preload_info[i].addr, wine_main_preload_info[i].size, PROT_NONE,
             MAP_FIXED | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1, 0);
    }
}

#else

/* the preloader will set these variables */
__attribute((visibility("default"))) struct r_debug *wine_r_debug = NULL;
const __attribute((visibility("default"))) struct wine_preload_info *wine_main_preload_info = NULL;

static void init_reserved_areas(void)
{
}

#endif

/* canonicalize path and return its directory name */
static char *realpath_dirname( const char *name )
{
    char *p, *fullpath = realpath( name, NULL );

    if (fullpath)
    {
        p = strrchr( fullpath, '/' );
        if (p == fullpath) p++;
        if (p) *p = 0;
    }
    return fullpath;
}

/* if string ends with tail, remove it */
static char *remove_tail( const char *str, const char *tail )
{
    size_t len = strlen( str );
    size_t tail_len = strlen( tail );
    char *ret;

    if (len < tail_len) return NULL;
    if (strcmp( str + len - tail_len, tail )) return NULL;
    ret = malloc( len - tail_len + 1 );
    memcpy( ret, str, len - tail_len );
    ret[len - tail_len] = 0;
    return ret;
}

/* build a path from the specified dir and name */
static char *build_path( const char *dir, const char *name )
{
    size_t len = strlen( dir );
    char *ret = malloc( len + strlen( name ) + 2 );

    memcpy( ret, dir, len );
    if (len && ret[len - 1] != '/') ret[len++] = '/';
    strcpy( ret + len, name );
    return ret;
}

static char *dup_trimmed_string( const char *str, size_t len )
{
    char *ret;

    while (len && isspace( (unsigned char)*str ))
    {
        str++;
        len--;
    }
    while (len && isspace( (unsigned char)str[len - 1] )) len--;

    if (!(ret = malloc( len + 1 ))) return NULL;
    memcpy( ret, str, len );
    ret[len] = 0;
    return ret;
}

static int process_env_bases_contains( const char *bases, const char *name )
{
    size_t name_len = strlen( name );

    if (!bases || !*bases) return 0;

    while (*bases)
    {
        const char *next = strchr( bases, ';' );
        size_t key_len = strcspn( bases, "=;" );

        if (key_len == name_len && !strncasecmp( bases, name, name_len )) return 1;
        if (!next) break;
        bases = next + 1;
    }
    return 0;
}

static char *append_process_env_base( char *bases, const char *name )
{
    const char *value = getenv( name );
    size_t bases_len = bases ? strlen( bases ) : 0;
    size_t name_len = strlen( name );
    size_t value_len = value ? strlen( value ) + 1 : 0;
    char *ret;

    if (!(ret = realloc( bases, bases_len + (bases_len ? 1 : 0) + name_len + value_len + 1 )))
    {
        free( bases );
        return NULL;
    }

    if (bases_len) ret[bases_len++] = ';';
    memcpy( ret + bases_len, name, name_len );
    bases_len += name_len;
    if (value)
    {
        ret[bases_len++] = '=';
        memcpy( ret + bases_len, value, value_len - 1 );
        bases_len += value_len - 1;
    }
    ret[bases_len] = 0;
    return ret;
}

static int record_process_env_base( char **bases, const char *name )
{
    if (!*bases)
    {
        const char *current = getenv( per_process_env_bases_var );
        if (current && !(*bases = strdup( current ))) return 0;
    }
    if (process_env_bases_contains( *bases, name )) return 1;
    return (*bases = append_process_env_base( *bases, name )) != NULL;
}

static void restore_process_env_bases(void)
{
    const char *bases = getenv( per_process_env_bases_var );
    char *copy, *entry, *saveptr;

    if (!bases || !*bases || !(copy = strdup( bases ))) return;

    for (entry = strtok_r( copy, ";", &saveptr ); entry; entry = strtok_r( NULL, ";", &saveptr ))
    {
        char *eq = strchr( entry, '=' );

        if (eq)
        {
            *eq = 0;
            setenv( entry, eq + 1, 1 );
        }
        else unsetenv( entry );
    }
    free( copy );
}

static void trim_line_end( char *str )
{
    size_t len = strlen( str );

    while (len && (str[len - 1] == '\n' || str[len - 1] == '\r')) str[--len] = 0;
}

static const char *get_basename( const char *path )
{
    const char *base = path, *p;

    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

static char *find_process_env_file( const char *image )
{
    const char *dir = getenv( per_process_env_dir_var );
    const char *base;
    char *env_name, *ret = NULL;
    size_t len;

    if (!dir || !*dir || !image || !*image) return NULL;

    base = get_basename( image );
    len = strlen( base );
    if (!(env_name = malloc( len + sizeof(".env") ))) return NULL;
    memcpy( env_name, base, len );
    memcpy( env_name + len, ".env", sizeof(".env") );

    if ((ret = build_path( dir, env_name )) && !access( ret, R_OK ))
    {
        free( env_name );
        return ret;
    }

    free( ret );
    ret = NULL;

    {
        DIR *handle;
        struct dirent *entry;

        if ((handle = opendir( dir )))
        {
            while ((entry = readdir( handle )))
            {
                if (!strcasecmp( entry->d_name, env_name ))
                {
                    ret = build_path( dir, entry->d_name );
                    break;
                }
            }
            closedir( handle );
        }
    }

    free( env_name );
    return ret;
}

static void apply_process_env_file( const char *image )
{
    char *bases = NULL, *path, *line = NULL;
    size_t capacity = 0;
    FILE *file;

    if (!(path = find_process_env_file( image ))) return;
    if (!(file = fopen( path, "r" )))
    {
        free( path );
        return;
    }

    while (getline( &line, &capacity, file ) != -1)
    {
        char *entry = line;
        char *eq, *name;

        trim_line_end( entry );
        while (*entry && isspace( (unsigned char)*entry )) entry++;
        if (!*entry || *entry == '#') continue;

        if (!strncasecmp( entry, "unset ", 6 ))
        {
            if ((name = dup_trimmed_string( entry + 6, strlen( entry + 6 ) )))
            {
                if (*name && !record_process_env_base( &bases, name ))
                {
                    free( name );
                    break;
                }
                if (*name) unsetenv( name );
                free( name );
            }
            continue;
        }

        if (!strncasecmp( entry, "export ", 7 )) entry += 7;
        if (!(eq = strchr( entry, '=' ))) continue;

        if (!(name = dup_trimmed_string( entry, eq - entry ))) break;
        if (*name)
        {
            char *value = eq + 1;

            if (!record_process_env_base( &bases, name ))
            {
                free( name );
                break;
            }
            while (*value && isspace( (unsigned char)*value )) value++;
            setenv( name, value, 1 );
        }
        free( name );
    }

    if (bases && *bases) setenv( per_process_env_bases_var, bases, 1 );
    free( line );
    free( bases );
    fclose( file );
    free( path );
}

/* build a path with the relative dir from 'from' to 'dest' appended to base */
static char *build_relative_path( const char *base, const char *from, const char *dest )
{
    const char *start;
    char *ret;
    unsigned int dotdots = 0;

    for (;;)
    {
        while (*from == '/') from++;
        while (*dest == '/') dest++;
        start = dest;  /* save start of next path element */
        if (!*from) break;

        while (*from && *from != '/' && *from == *dest) { from++; dest++; }
        if ((!*from || *from == '/') && (!*dest || *dest == '/')) continue;

        do  /* count remaining elements in 'from' */
        {
            dotdots++;
            while (*from && *from != '/') from++;
            while (*from == '/') from++;
        }
        while (*from);
        break;
    }

    ret = malloc( strlen(base) + 3 * dotdots + strlen(start) + 2 );
    strcpy( ret, base );
    while (dotdots--) strcat( ret, "/.." );

    if (!start[0]) return ret;
    strcat( ret, "/" );
    strcat( ret, start );
    return ret;
}

static const char *get_self_exe( char *argv0 )
{
#if defined(__linux__) || defined(__FreeBSD_kernel__) || defined(__NetBSD__)
    return "/proc/self/exe";
#elif defined (__FreeBSD__) || defined(__DragonFly__)
    static int pathname[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t path_size = PATH_MAX;
    char *path = malloc( path_size );
    if (path && !sysctl( pathname, sizeof(pathname)/sizeof(pathname[0]), path, &path_size, NULL, 0 ))
        return path;
    free( path );
#endif

    if (!strchr( argv0, '/' )) /* search in PATH */
    {
        char *p, *path = getenv( "PATH" );

        if (!path || !(path = strdup(path))) return NULL;
        for (p = strtok( path, ":" ); p; p = strtok( NULL, ":" ))
        {
            char *name = build_path( p, argv0 );
            if (!access( name, X_OK ))
            {
                free( path );
                return name;
            }
            free( name );
        }
        free( path );
        return NULL;
    }
    return argv0;
}

static void *try_dlopen( const char *dir, const char *name )
{
    char *path = build_path( dir, name );
    void *handle = dlopen( path, RTLD_NOW );
    free( path );
    return handle;
}

static void *load_ntdll( char *argv0 )
{
#ifdef __i386__
#define SO_DIR "i386-unix/"
#elif defined(__x86_64__)
#define SO_DIR "x86_64-unix/"
#elif defined(__arm__)
#define SO_DIR "arm-unix/"
#elif defined(__aarch64__)
#define SO_DIR "aarch64-unix/"
#else
#define SO_DIR ""
#endif
    const char *self = get_self_exe( argv0 );
    char *path, *p;
    void *handle = NULL;

    if (self && ((path = realpath_dirname( self ))))
    {
        if ((p = remove_tail( path, "/loader" )))
            handle = try_dlopen( p, "dlls/ntdll/ntdll.so" );
        else if ((p = build_relative_path( path, BINDIR, LIBDIR )))
            handle = try_dlopen( p, "wine/" SO_DIR "ntdll.so" );
        free( p );
        free( path );
    }

    if (!handle && (path = getenv( "WINEDLLPATH" )))
    {
        path = strdup( path );
        for (p = strtok( path, ":" ); p; p = strtok( NULL, ":" ))
        {
            handle = try_dlopen( p, SO_DIR "ntdll.so" );
            if (!handle) handle = try_dlopen( p, "ntdll.so" );
            if (handle) break;
        }
        free( path );
    }

    if (!handle && !self) handle = try_dlopen( LIBDIR, "wine/" SO_DIR "ntdll.so" );

    return handle;
}


/**********************************************************************
 *           main
 */
int main( int argc, char *argv[] )
{
    void *handle;

    init_reserved_areas();
    restore_process_env_bases();
    if (argc > 1) apply_process_env_file( argv[1] );

    if ((handle = load_ntdll( argv[0] )))
    {
        void (*init_func)(int, char **) = dlsym( handle, "__wine_main" );
        if (init_func) init_func( argc, argv );
        fprintf( stderr, "wine: __wine_main function not found in ntdll.so\n" );
        exit(1);
    }

    fprintf( stderr, "wine: could not load ntdll.so: %s\n", dlerror() );
    pthread_detach( pthread_self() );  /* force importing libpthread for OpenGL */
    exit(1);
}
