// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2021 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

package com.tencent.yolov8ncnn;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.os.Handler;
import android.os.Vibrator;
import android.speech.tts.TextToSpeech;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.Button;
import android.widget.Spinner;

import java.io.File;
import java.util.Locale;

import android.support.v4.app.ActivityCompat;
import android.support.v4.content.ContextCompat;
public class MainActivity extends Activity implements SurfaceHolder.Callback {

    public static final int REQUEST_CAMERA = 100;

    private Yolov8Ncnn yolov8ncnn = new Yolov8Ncnn();
    private int facing = 0; // backward camera

    private Spinner spinnerModel;
    private Spinner spinnerCPUGPU;
    private int current_model = 0; // yolov8n
    private int current_cpugpu = 0; // CPU

    private SQLiteDatabase db;
    private SurfaceView cameraView;

    private TextToSpeech mTTS; // TextToSpeech 实例
    private static MainActivity instance;

    public native String getDetectionString();

    // 在你的 MainActivity 类中添加一个 Handler 对象
    private Handler mHandler;
    private Runnable mRunnable;

    public void vibrate(long milliseconds) {
        Vibrator vibrator = (Vibrator) getSystemService(Context.VIBRATOR_SERVICE);
        if (vibrator != null && vibrator.hasVibrator()) {
            vibrator.vibrate(milliseconds);
        }
    }

    //private void speak(String text) {
    //mTTS.speak(text, TextToSpeech.QUEUE_FLUSH, null);
    //}
    private void speak(String text) {
        Log.d("MainActivity", "speak: " + text);
        Bundle params = new Bundle();
        params.putFloat(TextToSpeech.Engine.KEY_PARAM_VOLUME, 1.0f); // set volume
        String utteranceId = "utteranceId";
        mTTS.speak(text, TextToSpeech.QUEUE_FLUSH, params, utteranceId);
    }

    public void speakPublic(String text) {
        Log.d("MainActivity", "speakPublic: " + text);
        speak(text);
    }

    /**
     * Called when the activity is first created.
     */
    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mHandler = new Handler();
        mRunnable = new Runnable() {
            @Override
            public void run() {
                checkAndProcessDistances();
                mHandler.postDelayed(this, 5000);  // Run this every 5 seconds
            }
        };
        mHandler.post(mRunnable);  // Start the timer

        setContentView(R.layout.main);
        // 初始化 TextToSpeech
        mTTS = new TextToSpeech(this, new TextToSpeech.OnInitListener() {
            @Override
            public void onInit(int status) {
                if (status == TextToSpeech.SUCCESS) {
                    int result = mTTS.setLanguage(Locale.US);

                    if (result == TextToSpeech.LANG_MISSING_DATA
                            || result == TextToSpeech.LANG_NOT_SUPPORTED) {
                        Log.e("TTS", "Language not supported");
                    } else {
                        mTTS.setPitch(1.0f);
                        mTTS.setSpeechRate(1.0f);
                        // 在这里进行测试
                        speak("This is smart camera for blind shopping");  // 这里是你想要测试的文本
                    }
                } else {
                    Log.e("TTS", "Initialization failed");
                }
            }
        });

        db = SQLiteDatabase.openDatabase("/storage/emulated/0/blind/objects.db", null, SQLiteDatabase.OPEN_READWRITE);
//语音实现
        processDistances();

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        cameraView = (SurfaceView) findViewById(R.id.cameraview);

        cameraView.getHolder().setFormat(PixelFormat.RGBA_8888);
        cameraView.getHolder().addCallback(this);

        // ...其它代码
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        cameraView = (SurfaceView) findViewById(R.id.cameraview);

        cameraView.getHolder().setFormat(PixelFormat.RGBA_8888);
        cameraView.getHolder().addCallback(this);

        Button buttonSwitchCamera = (Button) findViewById(R.id.buttonSwitchCamera);
        buttonSwitchCamera.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View arg0) {

                int new_facing = 1 - facing;

                yolov8ncnn.closeCamera();

                yolov8ncnn.openCamera(new_facing);

                facing = new_facing;
            }
        });

        spinnerModel = (Spinner) findViewById(R.id.spinnerModel);
        spinnerModel.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> arg0, View arg1, int position, long id) {
                if (position != current_model) {
                    current_model = position;

                    reload();
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> arg0) {
            }
        });

        spinnerCPUGPU = (Spinner) findViewById(R.id.spinnerCPUGPU);
        spinnerCPUGPU.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> arg0, View arg1, int position, long id) {
                if (position != current_cpugpu) {
                    current_cpugpu = position;
                    reload();
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> arg0) {
            }
        });

        Button buttonImage = (Button) findViewById(R.id.buttonImage);
        buttonImage.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View arg0) {
                Intent intent = new Intent(MainActivity.this, ImageSelectActivity.class);
                startActivity(intent);

            }
        });

        reload();
    }

    public static MainActivity getInstance() {
        return instance;
    }

    private void reload() {
        boolean ret_init = yolov8ncnn.loadModel(getAssets(), current_model, current_cpugpu);
        if (!ret_init) {
            Log.e("MainActivity", "yolov8ncnn loadModel failed");
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        yolov8ncnn.setOutputWindow(holder.getSurface());
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
    }

    @Override
    public void onResume() {
        super.onResume();

        if (ContextCompat.checkSelfPermission(getApplicationContext(), Manifest.permission.CAMERA) == PackageManager.PERMISSION_DENIED) {
            ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.CAMERA}, REQUEST_CAMERA);
        }

        yolov8ncnn.openCamera(facing);
    }

    @Override
    public void onPause() {
        super.onPause();

        yolov8ncnn.closeCamera();
    }

    @Override
    protected void onDestroy() {

        // 在这里进行资源的清理操作
        // 例如，如果您使用了 TextToSpeech，那么可以在这里将其关闭：

        if (mTTS != null) {
            mTTS.stop();
            mTTS.shutdown();
        }
        super.onDestroy();
    }

    public void checkAndProcessDistances() {
        File dbFile = new File("/storage/emulated/0/blind/objects.db");
        if (dbFile.exists() && !dbFile.isDirectory()) {
            String countQuery = "SELECT COUNT(*) FROM OBJECTS";
            Cursor cursor = db.rawQuery(countQuery, null);
            if (cursor != null && cursor.moveToFirst()) {
                int count = cursor.getInt(0);
                cursor.close();
                if (count % 20 == 0) {
                    processDistances();
                }
            }
        } else {
            // Do something or nothing if the database does not exist.
        }
    }

    public static String getNthDistance(SQLiteDatabase db, int n) {
        File dbFile = new File("/storage/emulated/0/blind/objects.db");
        if (dbFile.exists() && !dbFile.isDirectory()) {
            String query = "SELECT DISTANCE FROM OBJECTS ORDER BY ID DESC LIMIT 1 OFFSET " + n; // Offset starts from 0
            Cursor cursor = db.rawQuery(query, null);

            if (cursor != null && cursor.moveToFirst()) {
                float distance = cursor.getFloat(0);
                int intDistance = (int) distance;
                String formattedDistance = String.valueOf(intDistance);
                cursor.close();
                return formattedDistance;
            }
        } else {
            // Do something or nothing if the database does not exist.
        }

        return null;
    }

    public void processDistances() {
        File dbFile = new File("/storage/emulated/0/blind/objects.db");
        if (dbFile.exists() && !dbFile.isDirectory()) {
            String distance = getNthDistance(db, 19); // Here, we directly pass 19 as the record position
            if (distance != null) {
                String contents = distance + " centimetres";
                speak(contents);
            } else {
                speak("No objects");
            }}}
    }

