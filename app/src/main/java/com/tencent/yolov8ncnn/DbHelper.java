package com.tencent.yolov8ncnn;

import android.database.sqlite.SQLiteDatabase;
import android.database.Cursor;

public class DbHelper {

    public static String getNthDistance(SQLiteDatabase db, int i) {
        String query = "SELECT DISTANCE FROM OBJECTS ORDER BY ID DESC LIMIT 1 OFFSET 14"; // 14 because indexing starts at 0
        Cursor cursor = db.rawQuery(query, null);

        if (cursor != null && cursor.moveToFirst()) {
            String distance = cursor.getString(0);
            cursor.close();
            return distance;
        }

        return null;
    }
}
