package com.aitest.aicli;

import android.app.Activity;
import android.app.Application;
import android.os.Bundle;

/** Keeps update work alive across rotation and the system install-permission page. */
public final class UpdateApplication extends Application {
    private AppUpdater updater;

    static AppUpdater updater(Activity activity) {
        return ((UpdateApplication) activity.getApplication()).updater;
    }

    @Override public void onCreate() {
        super.onCreate();
        updater = new AppUpdater(this);
        registerActivityLifecycleCallbacks(new ActivityLifecycleCallbacks() {
            @Override public void onActivityResumed(Activity activity) {
                if (activity instanceof MainActivity || activity instanceof SettingsActivity)
                    updater.resume(activity);
            }
            @Override public void onActivityPaused(Activity activity) { updater.pause(activity); }
            @Override public void onActivityCreated(Activity activity, Bundle state) { }
            @Override public void onActivityStarted(Activity activity) { }
            @Override public void onActivityStopped(Activity activity) { }
            @Override public void onActivitySaveInstanceState(Activity activity, Bundle state) { }
            @Override public void onActivityDestroyed(Activity activity) { updater.pause(activity); }
        });
    }
}
