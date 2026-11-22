package me.speleologist.yacsi.scan

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import androidx.core.content.edit
import androidx.core.net.toUri
import java.io.BufferedWriter
import java.io.IOException
import java.io.OutputStreamWriter

/**
 * The folder run files are written to. The user picks it once through the
 * system folder picker; access is kept across restarts.
 */
class RunStorage(context: Context) {
    private val resolver = context.contentResolver
    private val prefs = context.getSharedPreferences("runs", Context.MODE_PRIVATE)

    /** The chosen folder, if access to it is still granted. */
    fun folderUri(): Uri? {
        val uri = prefs.getString(KEY_FOLDER, null)?.toUri() ?: return null
        val granted = resolver.persistedUriPermissions.any { it.uri == uri && it.isWritePermission }
        return if (granted) uri else null
    }

    fun rememberFolder(treeUri: Uri) {
        resolver.takePersistableUriPermission(
            treeUri,
            Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
        )
        prefs.edit { putString(KEY_FOLDER, treeUri.toString()) }
    }

    fun folderName(treeUri: Uri): String =
        resolver.query(folderDocument(treeUri), arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),
            null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        } ?: treeUri.lastPathSegment ?: "folder"

    /** Creates [fileName] in the folder and opens it for writing. */
    fun createRun(treeUri: Uri, fileName: String): RunWriter {
        val uri = DocumentsContract.createDocument(resolver, folderDocument(treeUri), "text/csv", fileName)
            ?: throw IOException("Cannot create $fileName")
        val stream = resolver.openOutputStream(uri, "w") ?: throw IOException("Cannot open $fileName")
        return RunWriter(fileName, BufferedWriter(OutputStreamWriter(stream, Charsets.UTF_8)))
    }

    private fun folderDocument(treeUri: Uri): Uri =
        DocumentsContract.buildDocumentUriUsingTree(treeUri, DocumentsContract.getTreeDocumentId(treeUri))

    private companion object {
        const val KEY_FOLDER = "folder"
    }
}

/** An open run file; every write is flushed so a crash or dropout loses nothing written. */
class RunWriter(val fileName: String, private val writer: BufferedWriter) {
    var rows = 0
        private set

    fun writeHeader(text: String) {
        writer.write(text)
        writer.flush()
    }

    fun writeRow(text: String) {
        writer.write(text)
        writer.flush()
        rows++
    }

    fun close() {
        writer.close()
    }
}
