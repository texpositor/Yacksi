package me.speleologist.yacsi.voice

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioManager
import android.media.ToneGenerator
import android.util.Log
import androidx.core.content.ContextCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import org.json.JSONArray
import org.json.JSONObject
import org.vosk.Model
import org.vosk.Recognizer
import org.vosk.android.RecognitionListener
import org.vosk.android.SpeechService
import org.vosk.android.StorageService

data class VoiceState(
    val isInitialized: Boolean = false,
    val isListening: Boolean = false,
    val lastRecognizedText: String = "",
    val lastAction: VoiceAction? = null,
    val statusMessage: String = "Voice inactive"
)

class VoiceCommandManager(private val context: Context) : RecognitionListener {

    private val _state = MutableStateFlow(VoiceState())
    val state: StateFlow<VoiceState> = _state.asStateFlow()

    private var speechService: SpeechService? = null
    private var model: Model? = null

    // Audible confirmation that a command was recognised; uses the media volume stream
    private var toneGenerator: ToneGenerator? = try {
        ToneGenerator(AudioManager.STREAM_MUSIC, ToneGenerator.MAX_VOLUME)
    } catch (e: RuntimeException) {
        Log.e("VoiceCommandManager", "Failed to create tone generator", e)
        null
    }

    var onCommandRecognized: ((VoiceAction) -> Unit)? = null

    fun initialize() {
        if (_state.value.isInitialized) return

        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            _state.value = _state.value.copy(statusMessage = "Microphone permission not granted")
            return
        }

        _state.value = _state.value.copy(statusMessage = "Loading voice model...")

        StorageService.unpack(context, "model-en-gb", "model", { loadedModel ->
            model = loadedModel
            _state.value = _state.value.copy(
                isInitialized = true,
                statusMessage = "Voice active: 'yak see leg / section / left 1234'"
            )
            startListening()
        }, { exception ->
            Log.e("VoiceCommandManager", "Failed to unpack model", exception)
            _state.value = _state.value.copy(
                statusMessage = "Voice model error: ${exception.message}"
            )
        })
    }

    fun startListening() {
        val loadedModel = model ?: return
        if (speechService != null) return

        try {
            // Restrict the recogniser to the command and number words, plus unknown
            val grammar = JSONArray(VOICE_GRAMMAR_WORDS + "[unk]").toString()
            val recognizer = Recognizer(loadedModel, 16000.0f, grammar)

            speechService = SpeechService(recognizer, 16000.0f).apply {
                startListening(this@VoiceCommandManager)
            }
            _state.value = _state.value.copy(
                isListening = true,
                statusMessage = "Listening for 'yak see leg / section / left 1234'..."
            )
        } catch (e: Exception) {
            Log.e("VoiceCommandManager", "Failed to start speech service", e)
            _state.value = _state.value.copy(
                isListening = false,
                statusMessage = "Speech error: ${e.message}"
            )
        }
    }

    fun stopListening() {
        speechService?.stop()
        speechService?.shutdown()
        speechService = null
        _state.value = _state.value.copy(
            isListening = false,
            statusMessage = "Voice paused"
        )
    }

    override fun onPartialResult(hypothesis: String?) {
        // Partials repeat many times per utterance; only act on final results
    }

    override fun onResult(hypothesis: String?) {
        processResultJson(hypothesis)
    }

    override fun onFinalResult(hypothesis: String?) {
        processResultJson(hypothesis)
    }

    override fun onError(exception: Exception?) {
        Log.e("VoiceCommandManager", "Vosk recognition error", exception)
    }

    override fun onTimeout() {
        speechService?.cancel()
        speechService = null
        if (_state.value.isInitialized) {
            startListening()
        }
    }

    private fun processResultJson(json: String?) {
        if (json.isNullOrBlank()) return
        try {
            val jsonObject = JSONObject(json)
            val text = jsonObject.optString("text", "")
                .lowercase()
                .trim()

            if (text.isBlank()) return

            _state.value = _state.value.copy(lastRecognizedText = text)

            val action = parseVoiceCommand(text) ?: return
            // statusMessage is left alone; the caller announces recognised commands
            _state.value = _state.value.copy(lastAction = action)
            when (action) {
                // Single beep for leg and for an LRUD value, double beep for section
                VoiceAction.SetTargetLeg, is VoiceAction.SetLrud ->
                    toneGenerator?.startTone(ToneGenerator.TONE_PROP_BEEP, 200)
                VoiceAction.SetTargetSection ->
                    toneGenerator?.startTone(ToneGenerator.TONE_PROP_BEEP2, 400)
                // Error tone: a side was heard but the number wasn't
                is VoiceAction.MissingNumber ->
                    toneGenerator?.startTone(ToneGenerator.TONE_PROP_NACK, 400)
            }
            onCommandRecognized?.invoke(action)
        } catch (e: Exception) {
            Log.e("VoiceCommandManager", "Error parsing result JSON", e)
        }
    }

    fun destroy() {
        stopListening()
        model?.close()
        model = null
        toneGenerator?.release()
        toneGenerator = null
    }
}
