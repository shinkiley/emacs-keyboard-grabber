#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include "emacs-module.h"
#include "lib.h"

int plugin_is_GPL_compatible;

// Helper struct
struct xcb_state {
    xcb_screen_t *screen;
    xcb_window_t window;
    pid_t pid;
};

// Symbols
static emacs_value Qnil;
static emacs_value Qt;

// Global event structure
static xcb_generic_event_t *ev;
static xcb_key_press_event_t *kev;

// finalizers
static void
fin_close_xcb_state(void *ptr)
{
    if (ptr) {
        struct xcb_state* state = (struct xcb_state*) ptr;
        close_xcb_connection();
        free(state);
    }
}

// emacs-lisp like bindings
static void
signal_error(emacs_env *env, const char* message)
{
    emacs_value Qerror = env->intern(env, "error");
    emacs_value Qmessage = env->make_string(env, message, strlen(message));
    emacs_value args[] = { Qmessage };
    env->funcall(env, Qerror, 1, args);
}

static void
bindFunction (emacs_env *env, const char *name, emacs_value Qfun)
{
    emacs_value Qfset = env->intern (env, "fset");
    emacs_value Qsym = env->intern (env, name);

    emacs_value args[] = { Qsym, Qfun };

    env->funcall (env, Qfset, 2, args);
}

static void
setVariable(emacs_env *env, const char *name, emacs_value Qval)
{
    emacs_value Qset = env->intern(env, "set");
    emacs_value Qsym = env->intern(env, name);
    emacs_value args[] = { Qsym, Qval };

    env->funcall(env, Qset, 2, args);
}

static emacs_value
getVariable(emacs_env *env, const char *name) {
    emacs_value QsymbolValue = env->intern(env, "symbol-value");
    emacs_value Qsym = env->intern(env, name);
    emacs_value args[] = { Qsym };

    return env->funcall(env, QsymbolValue, 1, args);
}

static void
provide(emacs_env *env, const char *feature)
{
    emacs_value Qfeature = env->intern (env, feature);
    emacs_value Qprovide = env->intern (env, "provide");
    emacs_value args[] = { Qfeature };

    env->funcall (env, Qprovide, 1, args);
}

static emacs_value
makeVector(emacs_env *env, int length, int init) {
    emacs_value QmakeVector = env->intern(env, "make-vector");
    emacs_value Qlength = env->make_integer(env, length);
    emacs_value Qinit = env->make_integer(env, init);
    emacs_value args[] = { Qlength, Qinit };

    return env->funcall(env, QmakeVector, 2, args);
}

// exported functions
static emacs_value
FopenConnection(emacs_env *env, ptrdiff_t n, emacs_value *args, void *ptr) {
    (void)ptr;
    (void)n;
    struct xcb_state *state = calloc(1, sizeof(struct xcb_state));

    // find Emacs' pid
    state->pid = getpid();
    if (state->pid < 0) {
        free(state);
        signal_error(env, "Can't get pid of the Emacs process");
        return Qnil;
    }

    // find Emacs' window
    setup_display_and_screen(NULL);
    xcb_window_t* window_ptr = find_window_by_pid_from_root(state->pid);
    if (! window_ptr) {
        close_xcb_connection();
        free(state);
        signal_error(env, "Can't find Emacs window");
        return Qnil;
    }
    state->window = *window_ptr;
    free(window_ptr);

    return env->make_user_ptr(env, fin_close_xcb_state, (void*)state);
}

/* todo: ometimes leads to segfault :(*/
static emacs_value
FcloseConnection(emacs_env *env, ptrdiff_t n, emacs_value *args, void *ptr) {
    (void)ptr;
    (void)n;

    struct xcb_state *state = (struct xcb_state*)env->get_user_ptr(env, args[0]);
    close_xcb_connection();
    free(state);

    return Qt;
}

static emacs_value
FgrabKeyboard(emacs_env *env, ptrdiff_t n, emacs_value *args, void *ptr) {
    (void)ptr;
    (void)n;

    struct xcb_state *state = (struct xcb_state*)env->get_user_ptr(env, args[0]);
    grab_keyboard(state->window);

    return Qt;
}

static emacs_value
FungrabKeyboard(emacs_env *env, ptrdiff_t n, emacs_value *args, void *ptr) {
    (void)ptr;
    (void)n;

    struct xcb_state *state = (struct xcb_state*)env->get_user_ptr(env, args[0]);
    ungrab_keyboard(state->window);

    return Qt;
}

static emacs_value
FreadEvent(emacs_env *env, ptrdiff_t n, emacs_value *args, void *ptr) {
    (void)ptr;
    (void)n;

    emacs_value vec = getVariable(env, "kg-last-event");

    ev = read_xcb_event();
    if (ev) {
        switch (ev->response_type & ~0x80) {
        case XCB_KEY_PRESS:
            kev = (xcb_key_press_event_t*) ev;
            env->vec_set(env, vec, 0, env->make_integer(env, 0));
            env->vec_set(env, vec, 1, env->make_integer(env, (int)kev->detail));
            break;

        case XCB_KEY_RELEASE:
            kev = (xcb_key_press_event_t*) ev;
            env->vec_set(env, vec, 0, env->make_integer(env, 1));
            env->vec_set(env, vec, 1, env->make_integer(env, (int)kev->detail));
            break;
        }
        free(ev);
        return vec;
    }

    return Qnil;
};

int
emacs_module_init (struct emacs_runtime *ert)
{
    emacs_env *env = ert->get_environment (ert);

    Qnil = env->intern(env, "nil");
    Qt   = env->intern(env, "t");

    bindFunction(env,
                 "kg-open-connection",
                 env->make_function(env, 0, 0, FopenConnection, "doc", NULL));
    bindFunction(env,
                 "kg-close-connection",
                 env->make_function(env, 1, 1, FcloseConnection, "doc", NULL));
    bindFunction(env,
                 "kg-grab-keyboard",
                 env->make_function(env, 1, 1, FgrabKeyboard, "doc", NULL));
    bindFunction(env,
                 "kg-ungrab-keyboard",
                 env->make_function(env, 1, 1, FungrabKeyboard, "doc", NULL));
    bindFunction(env,
                 "kg-read-event",
                 env->make_function(env, 0, 0, FreadEvent, "doc", NULL));

    setVariable(env,
                "kg-key-press",
                env->make_integer(env, 0));
    setVariable(env,
                "kg-key-release",
                env->make_integer(env, 1));
    setVariable(env,
                "kg-key-repeat",
                env->make_integer(env, 2));
    setVariable(env,
                "kg-last-event",
                makeVector(env, 2, 0));

    provide(env, "keyboard-grabber");

    // loaded successfully
    return 0;
}
