package com.tencent.yolov8ncnn;

import android.content.Context;
import android.os.Vibrator;

public class VibratorHelper {
    private Vibrator mVibrator;

    public VibratorHelper(Context context) {
        mVibrator = (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
    }

    public void vibrate(long milliseconds) {
        mVibrator.vibrate(milliseconds);
    }
}