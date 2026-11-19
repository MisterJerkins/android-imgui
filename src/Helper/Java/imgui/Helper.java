package imgui;

import android.content.ClipData;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.view.InputChannel;
import android.view.InputEvent;
import android.view.InputEventReceiver;
import android.view.MotionEvent;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;

/**
 * Does the few things only the framework can do. Commands come in on standard input, one per
 * line; with the argument "touches" it also reports every touch on the screen on standard output.
 * What it may do depends on the user it runs as: the clipboard answers the shell user, not root.
 */
public final class Helper {
    private static final int EVENT_TOUCH = 2;
    private static final int MAIN_DISPLAY = 0;
    private static final String SHELL = "com.android.shell";

    private static Object power;
    private static Object clipboard;

    public static void main(String[] arguments) throws Exception {
        say("ready");
        // The process may only talk to the system until this method returns, so it never
        // returns while there is work: either it watches touches or it waits for commands.
        if (arguments.length > 0 && arguments[0].equals("touches")) {
            new Thread(Helper::listen).start();
            attempt(Helper::watch);
            System.exit(1);
        } else {
            listen();
        }
    }

    /** Carries out commands until the other end goes away, then ends the process. */
    private static void listen() {
        try {
            BufferedReader lines = new BufferedReader(new InputStreamReader(System.in));
            for (String line = lines.readLine(); line != null; line = lines.readLine()) {
                String[] command = line.split(" ", 2);
                if (command[0].equals("activity")) {
                    attempt(Helper::activity);
                } else if (command[0].equals("copy") && command.length > 1) {
                    attempt(() -> clipboard("setPrimaryClip", ClipData.newPlainText(null, text(command[1]))));
                } else if (command[0].equals("paste") && command.length > 1) {
                    String[] found = {""};
                    attempt(() -> found[0] = pasted());
                    say("clip " + command[1] + found[0]);
                }
            }
        } catch (IOException gone) {
            // The other end is gone, which is how this ends.
        }
        System.exit(0);
    }

    private interface Action {
        void run() throws Exception;
    }

    private static void attempt(Action action) {
        try {
            action.run();
        } catch (Exception failure) {
            say("error " + (failure instanceof InvocationTargetException ? failure.getCause() : failure));
        }
    }

    private static synchronized void say(String line) {
        System.out.println(line);
        System.out.flush();
    }

    private static Object service(String name, String type) throws Exception {
        Object binder = Class.forName("android.os.ServiceManager").getMethod("getService", String.class).invoke(null, name);
        return Class.forName(type + "$Stub").getMethod("asInterface", IBinder.class).invoke(null, binder);
    }

    /** Tells the system the user is there. The display id came first from Android 13 on. */
    private static void activity() throws Exception {
        if (power == null) {
            power = service("power", "android.os.IPowerManager");
        }
        long now = SystemClock.uptimeMillis();
        for (Method method : power.getClass().getMethods()) {
            if (!method.getName().equals("userActivity")) {
                continue;
            }
            if (method.getParameterTypes().length == 4) {
                method.invoke(power, MAIN_DISPLAY, now, EVENT_TOUCH, 0);
            } else {
                method.invoke(power, now, EVENT_TOUCH, 0);
            }
            return;
        }
    }

    /**
     * Calls the clipboard service. Its methods grew arguments from release to release, always in
     * the same order: the calling package, further names nobody has to give, the user, numbers
     * that are zero for the main display, and flags that say a person asked.
     */
    private static Object clipboard(String name, ClipData clip) throws Exception {
        if (clipboard == null) {
            clipboard = service("clipboard", "android.content.IClipboard");
        }
        Method shortest = null;
        for (Method method : clipboard.getClass().getMethods()) {
            if (method.getName().equals(name) && (shortest == null || method.getParameterTypes().length < shortest.getParameterTypes().length)) {
                shortest = method;
            }
        }
        if (shortest == null) {
            throw new NoSuchMethodException(name);
        }
        Class<?>[] types = shortest.getParameterTypes();
        Object[] values = new Object[types.length];
        boolean named = false;
        boolean placed = false;
        for (int index = 0; index < types.length; index++) {
            if (types[index] == ClipData.class) {
                values[index] = clip;
            } else if (types[index] == String.class) {
                values[index] = named ? null : SHELL;
                named = true;
            } else if (types[index] == int.class) {
                values[index] = placed ? 0 : user();
                placed = true;
            } else if (types[index] == boolean.class) {
                values[index] = true;
            }
        }
        return shortest.invoke(clipboard, values);
    }

    /** The user in front, whose clipboard is the one on screen. */
    private static int user() {
        try {
            return (Integer) Class.forName("android.app.ActivityManager").getMethod("getCurrentUser").invoke(null);
        } catch (Exception unknown) {
            return 0;
        }
    }

    /** The clipboard's text as a reply: empty without any, else a space and the text. */
    private static String pasted() throws Exception {
        ClipData clip = (ClipData) clipboard("getPrimaryClip", null);
        CharSequence text = clip == null || clip.getItemCount() == 0 ? null : clip.getItemAt(0).getText();
        if (text == null) {
            // Said for whoever looks into why a paste brought nothing; what kind of clip, never its content.
            say("note " + (clip == null ? "the clipboard gave no clip" : "the clip has no text: " + clip.getDescription()));
        }
        return text == null ? "" : " " + hex(text.toString());
    }

    /** Text travels as hexadecimal UTF-8, which keeps line breaks out of the line protocol. */
    private static String hex(String text) {
        StringBuilder digits = new StringBuilder();
        for (byte value : text.getBytes(StandardCharsets.UTF_8)) {
            digits.append(Character.forDigit(value >> 4 & 15, 16)).append(Character.forDigit(value & 15, 16));
        }
        return digits.toString();
    }

    private static String text(String hex) {
        byte[] bytes = new byte[hex.length() / 2];
        for (int index = 0; index < bytes.length; index++) {
            bytes[index] = (byte) Integer.parseInt(hex.substring(index * 2, index * 2 + 2), 16);
        }
        return new String(bytes, StandardCharsets.UTF_8);
    }

    /** Watches touches the way the system's own gesture detectors do, without taking them. */
    private static void watch() throws Exception {
        Looper.prepare();
        Object manager;
        try {
            manager = Class.forName("android.hardware.input.InputManagerGlobal").getMethod("getInstance").invoke(null);
        } catch (ReflectiveOperationException older) {
            manager = Class.forName("android.hardware.input.InputManager").getMethod("getInstance").invoke(null);
        }
        Object monitor = manager.getClass().getMethod("monitorGestureInput", String.class, int.class).invoke(manager, "android-imgui", MAIN_DISPLAY);
        InputChannel channel = (InputChannel) monitor.getClass().getMethod("getInputChannel").invoke(monitor);
        new Touches(channel, Looper.myLooper());
        say("watching");
        Looper.loop();
    }

    private static final class Touches extends InputEventReceiver {
        Touches(InputChannel channel, Looper looper) {
            super(channel, looper);
        }

        @Override
        public void onInputEvent(InputEvent event) {
            if (event instanceof MotionEvent) {
                MotionEvent motion = (MotionEvent) event;
                StringBuilder line = new StringBuilder("touch ").append(motion.getActionMasked()).append(' ').append(motion.getActionIndex());
                for (int pointer = 0; pointer < motion.getPointerCount(); pointer++) {
                    line.append(' ').append(motion.getPointerId(pointer)).append(' ').append(motion.getX(pointer)).append(' ').append(motion.getY(pointer));
                }
                say(line.toString());
            }
            finishInputEvent(event, false);
        }
    }
}
