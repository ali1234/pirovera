package com.robotfuzz.al.pirovera;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.TimeZone;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Environment;
import android.os.PowerManager;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.KeyEvent;
import android.view.View;
import android.view.View.OnClickListener;
import android.widget.ToggleButton;
import android.widget.ImageButton;
import android.widget.SeekBar;
import android.widget.SeekBar.OnSeekBarChangeListener;
import android.widget.TextView;
import android.widget.Toast;

import org.freedesktop.gstreamer.GStreamer;

import com.MobileAnarchy.Android.Widgets.Joystick.JoystickView;
import com.MobileAnarchy.Android.Widgets.Joystick.JoystickMovedListener;

public class PiRover extends Activity implements SurfaceHolder.Callback {
    private native void nativeInit();     // Initialize native code, build pipeline, etc
    private native void nativeFinalize(); // Destroy pipeline and shutdown native code
    private native void nativeSetUri(String uri); // Set the URI of the media to play
    private native void nativePlay();     // Set pipeline to PLAYING
    private native void nativePause();    // Set pipeline to PAUSED
    private native void nativeSetLeft(int n);  // Set left motor
    private native void nativeSetRight(int n); // Set right motor
    private native void nativeSetHeadlights(boolean n);  // Set left motor
    private native void nativeSetTaillights(boolean n);  // Set left motor
    private native void nativeSetHazardlights(boolean n);  // Set left motor
    private static native boolean nativeClassInit(); // Initialize native class: cache Method IDs for callbacks
    private native void nativeSurfaceInit(Object surface); // A new surface is available
    private native void nativeSurfaceFinalize(); // Surface about to be destroyed
    private long native_custom_data;      // Native code will use this to keep private data
    private JoystickView jvleft;
    private JoystickView jvright;

    private final String mediaUri = "rtsp://172.24.1.1:8554/test";

    private PowerManager.WakeLock wake_lock;

    private JoystickMovedListener _listenerLeft = new JoystickMovedListener() {

        @Override
        public void OnMoved(int pan, int tilt) {
            nativeSetLeft(tilt);
        }

        @Override
        public void OnReleased() {
            nativeSetLeft(0);
        }

        @Override
        public void OnReturnedToCenter() {
            nativeSetLeft(0);
        }

    };

    private JoystickMovedListener _listenerRight = new JoystickMovedListener() {

        @Override
        public void OnMoved(int pan, int tilt) {
            nativeSetRight(tilt);
        }

        @Override
        public void OnReleased() {
            nativeSetRight(0);
        }

        @Override
        public void OnReturnedToCenter() {
            nativeSetRight(0);
        }

    };

    public void onHeadlightsClicked(View view) {
        nativeSetHeadlights(((ToggleButton) view).isChecked());
    }

    public void onTaillightsClicked(View view) {
        nativeSetTaillights(((ToggleButton) view).isChecked());
    }

    public void onHazardlightsClicked(View view) {
        nativeSetHazardlights(((ToggleButton) view).isChecked());
    }

    public boolean dispatchGenericMotionEvent(MotionEvent ev) {
        int source = ev.getSource();

        if ((source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
             jvleft.synthesizeMoveEvent(ev.getAxisValue(MotionEvent.AXIS_X), ev.getAxisValue(MotionEvent.AXIS_Y));
            jvright.synthesizeMoveEvent(ev.getAxisValue(MotionEvent.AXIS_RX), ev.getAxisValue(MotionEvent.AXIS_RY));
            return true;
        }

        return false;
    }

    public boolean dispatchKeyEvent(KeyEvent ev) {
        int source = ev.getSource();

        if ((source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) {
            return true;
        }

        return false;
    }

    // Called when the activity is first created.
    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        super.onCreate(savedInstanceState);

        // Initialize GStreamer and warn if it fails
        try {
            GStreamer.init(this);
        } catch (Exception e) {
            Toast.makeText(this, e.getMessage(), Toast.LENGTH_LONG).show();
            finish();
            return;
        }

        setContentView(R.layout.main);

        PowerManager pm = (PowerManager) getSystemService(Context.POWER_SERVICE);
        wake_lock = pm.newWakeLock(PowerManager.FULL_WAKE_LOCK, "Pi Rover Controller");
        wake_lock.setReferenceCounted(false);

        SurfaceView sv = (SurfaceView) this.findViewById(R.id.surface_video);
        SurfaceHolder sh = sv.getHolder();
        sh.addCallback(this);

        nativeInit();

        jvleft  = (JoystickView)findViewById(R.id.joystickleft);
        jvright = (JoystickView)findViewById(R.id.joystickright);
         jvleft.setOnJoystickMovedListener(_listenerLeft);
        jvright.setOnJoystickMovedListener(_listenerRight);
    }

    protected void onSaveInstanceState (Bundle outState) {
    }

    protected void onDestroy() {
        nativeFinalize();
        if (wake_lock.isHeld())
            wake_lock.release();
        super.onDestroy();
    }

    // Called from native code. Native code calls this once it has created its pipeline and
    // the main loop is running, so it is ready to accept commands.
    private void onGStreamerInitialized () {
        Log.i ("GStreamer", "GStreamer initialized:");

        // Restore previous playing state
        nativeSetUri(mediaUri);
        nativePlay();
        wake_lock.acquire();
    }

    static {
        System.loadLibrary("gstreamer_android");
        System.loadLibrary("pirovera");
        nativeClassInit();
    }

    public void surfaceChanged(SurfaceHolder holder, int format, int width,
            int height) {
        Log.d("GStreamer", "Surface changed to format " + format + " width "
                + width + " height " + height);
        nativeSurfaceInit (holder.getSurface());
    }

    public void surfaceCreated(SurfaceHolder holder) {
        Log.d("GStreamer", "Surface created: " + holder.getSurface());
    }

    public void surfaceDestroyed(SurfaceHolder holder) {
        Log.d("GStreamer", "Surface destroyed");
        nativeSurfaceFinalize ();
    }

    // Called from native code when the size of the media changes or is first detected.
    // Inform the video surface about the new size and recalculate the layout.
    private void onMediaSizeChanged (int width, int height) {
        Log.i ("GStreamer", "Media size changed to " + width + "x" + height);
        final GStreamerSurfaceView gsv = (GStreamerSurfaceView) this.findViewById(R.id.surface_video);
        gsv.media_width = width;
        gsv.media_height = height;
        runOnUiThread(new Runnable() {
            public void run() {
                gsv.requestLayout();
            }
        });
    }

}
