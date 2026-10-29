package me.speleologist.yacsi.survey

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import androidx.core.content.edit
import androidx.core.net.toUri

/**
 * Reads and writes survey files the user picked through the system file picker,
 * and remembers the open survey so it is reopened when the app restarts.
 */
class SurveyStorage(context: Context) {
    private val resolver = context.contentResolver
    private val prefs = context.getSharedPreferences("survey", Context.MODE_PRIVATE)

    var lrudBeforeLeg: Boolean
        get() = prefs.getBoolean(KEY_LRUD_BEFORE_LEG, false)
        set(value) = prefs.edit { putBoolean(KEY_LRUD_BEFORE_LEG, value) }

    /** The survey open when the app last ran, if access to it is still granted. */
    fun lastSurveyUri(): Uri? {
        val uri = prefs.getString(KEY_URI, null)?.toUri() ?: return null
        val granted = resolver.persistedUriPermissions.any { it.uri == uri && it.isWritePermission }
        return if (granted) uri else null
    }

    fun remember(uri: Uri) {
        try {
            resolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
        } catch (_: SecurityException) {
            // Not persistable from this provider; the survey still works until the app closes.
        }
        prefs.edit { putString(KEY_URI, uri.toString()) }
    }

    fun displayName(uri: Uri): String =
        resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        } ?: uri.lastPathSegment ?: "survey"

    fun load(uri: Uri): SurveyData {
        val text = resolver.openInputStream(uri)?.use { it.readBytes().toString(Charsets.UTF_8) }
            ?: throw java.io.IOException("Cannot open survey file")
        return SurveyCsv.parse(text)
    }

    /** Rewrites the whole file; surveys are small and this also handles undo. */
    fun save(uri: Uri, data: SurveyData) {
        // "wt" truncates; plain "w" does not on some providers.
        resolver.openOutputStream(uri, "wt")?.use {
            it.write(SurveyCsv.format(data).toByteArray(Charsets.UTF_8))
        } ?: throw java.io.IOException("Cannot write survey file")
    }

    private companion object {
        const val KEY_URI = "uri"
        const val KEY_LRUD_BEFORE_LEG = "lrud_before_leg"
    }
}
