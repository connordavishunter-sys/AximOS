#include <aximos.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Makes a pretend input device through uinput, as a laptop has: a lid, a power
// button, brightness and volume keys, or a touchpad, and has it do one thing, for
// the tests, as virtual machines have none of these.

#define RES 40 // the touchpad's units a millimetre
#define WAIT_US 1500000 // for wm to find the device, and after, to take what it did

static int fd;

static void emit(int type, int code, int value) {
    struct input_event e = {.type = type, .code = code, .value = value};

    if (write(fd, &e, sizeof(e)) != sizeof(e))
        perror("inputsim");
}

static void sync_now(void) {
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(10000);
}

static bool make(const char *name, bool touchpad) {
    struct uinput_setup setup = {.id = {.bustype = BUS_VIRTUAL, .vendor = 1, .product = 1}};
    static const int keys[] = {KEY_POWER, KEY_BRIGHTNESSUP, KEY_BRIGHTNESSDOWN, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE};
    static const int pad[] = {BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP};

    if ((fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK)) < 0) {
        perror("/dev/uinput");
        return false;
    }
    snprintf(setup.name, sizeof(setup.name), "%s", name);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (size_t i = 0; !touchpad && i < sizeof(keys) / sizeof(keys[0]); i++)
        ioctl(fd, UI_SET_KEYBIT, keys[i]);
    ioctl(fd, UI_SET_EVBIT, EV_SW);
    ioctl(fd, UI_SET_SWBIT, SW_LID);
    for (size_t i = 0; touchpad && i < sizeof(pad) / sizeof(pad[0]); i++)
        ioctl(fd, UI_SET_KEYBIT, pad[i]);
    if (touchpad) {
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
        for (int axis = ABS_X; axis <= ABS_Y; axis++)
            ioctl(fd, UI_ABS_SETUP, &(struct uinput_abs_setup){axis, {.maximum = 100 * RES, .resolution = RES}});
    }
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("inputsim");
        return false;
    }
    usleep(WAIT_US);
    return true;
}

static void press(int key) {
    emit(EV_KEY, key, 1);
    sync_now();
    emit(EV_KEY, key, 0);
    sync_now();
}

// Fingers land in the middle, move by dx and dy millimetres in steps, and lift.
static void touch(int fingers, int dx, int dy, int steps) {
    static const int tool[] = {0, BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP};
    int x = 50 * RES, y = 50 * RES;

    emit(EV_KEY, BTN_TOUCH, 1);
    emit(EV_KEY, tool[fingers], 1);
    emit(EV_ABS, ABS_X, x);
    emit(EV_ABS, ABS_Y, y);
    sync_now();
    for (int i = 1; i <= steps; i++) {
        emit(EV_ABS, ABS_X, x + dx * RES * i / steps);
        emit(EV_ABS, ABS_Y, y + dy * RES * i / steps);
        sync_now();
    }
    emit(EV_KEY, BTN_TOUCH, 0);
    emit(EV_KEY, tool[fingers], 0);
    sync_now();
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: inputsim lid close|open\n"
                            "       inputsim key power|brightnessup|brightnessdown|volumeup|volumedown|mute\n"
                            "       inputsim tap [FINGERS]\n"
                            "       inputsim move|scroll DX DY\n"
                            "       inputsim swipe left|right|up\n"
                            "Makes a pretend lid, buttons or touchpad, and has it do one thing, for tests.\n"
                            "move and scroll go DX and DY millimetres, with one finger or two. Only root can.\n");

    static const struct { const char *name; int key; } named[] = {
        {"power", KEY_POWER}, {"brightnessup", KEY_BRIGHTNESSUP}, {"brightnessdown", KEY_BRIGHTNESSDOWN},
        {"volumeup", KEY_VOLUMEUP}, {"volumedown", KEY_VOLUMEDOWN}, {"mute", KEY_MUTE},
    };
    bool pad = argc > 1 && strcmp(argv[1], "lid") && strcmp(argv[1], "key");

    if (argc < 2 || !make(pad ? "inputsim touchpad" : "inputsim buttons", pad))
        return 1;
    if (!strcmp(argv[1], "lid") && argc == 3) {
        emit(EV_SW, SW_LID, !strcmp(argv[2], "close"));
        sync_now();
    } else if (!strcmp(argv[1], "key") && argc == 3) {
        for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
            if (!strcmp(argv[2], named[i].name))
                press(named[i].key);
        }
    } else if (!strcmp(argv[1], "tap")) {
        touch(argc > 2 && argv[2][0] == '2' ? 2 : 1, 0, 0, 1);
    } else if ((!strcmp(argv[1], "move") || !strcmp(argv[1], "scroll")) && argc == 4) {
        int dx, dy;

        sscanf(argv[2], "%d", &dx);
        sscanf(argv[3], "%d", &dy);
        usleep(200000); // so it isn't a tap
        touch(argv[1][0] == 'm' ? 1 : 2, dx, dy, 20);
    } else if (!strcmp(argv[1], "swipe") && argc == 3) {
        touch(3, argv[2][0] == 'l' ? -30 : argv[2][0] == 'r' ? 30 : 0, argv[2][0] == 'u' ? -30 : 0, 20);
    } else {
        fprintf(stderr, "inputsim: try inputsim --help\n");
    }
    usleep(WAIT_US);
    ioctl(fd, UI_DEV_DESTROY);
    return 0;
}
