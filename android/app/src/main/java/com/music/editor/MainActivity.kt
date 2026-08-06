package com.music.editor

import android.app.AlertDialog
import android.content.ContentValues
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.provider.OpenableColumns
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.UnknownHostException
import java.net.URL
import java.util.concurrent.Executors

/**
 * 简谱编辑器 (C++ 后端)
 *
 * 把 music_editor 的成果移植到安卓:
 *  - RCP 文本编辑
 *  - C++ 后端渲染 PCM (AudioTrack 播放)
 *  - C++ 后端渲染 WAV (导出到下载目录)
 */
class MainActivity : AppCompatActivity() {

    companion object {
        private const val SAMPLE_RATE = 44100
        private const val KEY_BASE_URL = "ai_base_url"
        private const val KEY_API_KEY = "ai_api_key"
        private const val KEY_MODEL = "ai_model"
        private const val KEY_REASONING_EFFORT = "ai_reasoning_effort"
        private const val DEFAULT_MODEL = "deepseek-chat"

        // 生成谱: 只输出 RCP 文本本身 (第1行头部, 第2行音色, 其后音符)
        private const val SCORE_SYSTEM_PROMPT =
            "你是简谱(数字谱)创作专家。根据用户要求生成 RCP 格式简谱，只输出 RCP 内容本身，不要任何解释、前后缀、代码块标记或歌词。\n" +
            "RCP 由三部分组成，格式严格遵守：\n" +
            "第 1 行头部：BPM,基准频率(Hz),标准拍长(秒)。这三个值由你根据用户要求自行选定并保证自洽：\n" +
            "  - BPM 贴合情绪：缓慢抒情 60~90，从容中速 90~120，欢快激昂 120~180。\n" +
            "  - 基准频率决定调性（1=do 的音高），如 C=261.63、D=293.66、F=349.23，一般取 200~440；用户指定调性时按用户要求。\n" +
            "  - 标准拍长 = 60/BPM（一个四分音符的秒数），不要另外编一个对不上的数。\n" +
            "  - 三者的组合要让总时长大致符合用户要求（如\"30秒左右\"）。\n" +
            "第 2 行音色：一组谐波振幅，用英文逗号分隔，第 1 个为基频(1倍频)振幅、第 2 个为 2 倍频振幅…依此类推，数值 0~1、可含小数，3~12 个，应贴合要求的情绪与风格。" +
            "注意：振幅不必随倍频升高而递减，高倍频可以高于低倍频（如明亮、尖锐、鼻音等音色常强化高次谐波，第一个即基频通常可保持为 1）。\n" +
            "从第 3 行开始是音符，每行音符之间只用空格分隔，禁止 |、逗号、括号、连字符、歌词等任何其他符号。\n" +
            "音符格式：音级.八度标记+拍长分母\n" +
            "- 音级：1-7（do re mi fa sol la si），0 为休止。\n" +
            "- 八度标记（必须写，不能省略）：0=中音，+=高八度，-=低八度。\n" +
            "- 拍长分母：4=四分音符(1拍)、2=二分音符(2拍)、8=八分音符(半拍)、1=全音符(4拍)、16=十六分音符(四分之一拍)。\n" +
            "- 例：5.04=5音中音1拍；5.08=5音中音半拍；3.+4=mi高八度1拍；1.-4=do低八度1拍；0.04=休止1拍；0.08=休止半拍。\n" +
            "- 长音换更小的分母数字（如 5.02=2拍、5.01=4拍），不要用 - 或任何延长记号。\n" +
            "示例（欢快风格）：\n" +
            "120,392,0.25\n" +
            "1,0.7,0.5,0.3,0.2,0.1\n" +
            "1.+8 3.+8 5.+8 6.+8 5.+8 3.+8 1.+4 0.04\n" +
            "2.+8 4.+8 6.+8 7.+8 6.+8 4.+8 2.+4 0.04\n" +
            "5.04 5.08 6.08 7.08 1.+4 0.04\n" +
            "创作要求：旋律、BPM、调性(基准频率)与音色都要贴合用户要求的情绪、风格与大致时长；" +
            "欢快旋律多用 + 高八度音（1.+8、3.+8、5.+8 等）和 8/16 分音符，避免长音和低音。"
    }

    private lateinit var editor: EditText
    private lateinit var promptInput: EditText
    private lateinit var chkIncludeContext: CheckBox
    private lateinit var btnGenScore: Button
    private lateinit var btnImport: Button
    private lateinit var btnSave: Button
    private lateinit var btnPlay: Button
    private lateinit var btnStop: Button
    private lateinit var btnExport: Button
    private lateinit var statusText: TextView

    private val prefs by lazy { getSharedPreferences("ai", MODE_PRIVATE) }

    /** 内部存储中的当前乐谱文件 */
    private val savedScoreFile: File
        get() = File(filesDir, "scores/current.rcp")

    private val executor = Executors.newSingleThreadExecutor()
    private val mainHandler = Handler(Looper.getMainLooper())

    private val importPicker = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri ->
        uri?.let { importRcp(it) }
    }

    @Volatile
    private var playing = false
    @Volatile
    private var track: AudioTrack? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // 顶栏标题
        supportActionBar?.title = getString(R.string.title)

        // 处理系统栏 insets (edge-to-edge 下避免被状态栏/导航栏遮挡)
        val root = findViewById<View>(R.id.root)
        ViewCompat.setOnApplyWindowInsetsListener(root) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            val base = resources.getDimensionPixelSize(R.dimen.screen_padding)
            v.setPadding(
                base + bars.left,
                base + bars.top,
                base + bars.right,
                base + bars.bottom
            )
            insets
        }

        editor = findViewById(R.id.editor)
        promptInput = findViewById(R.id.prompt_input)
        chkIncludeContext = findViewById(R.id.chk_include_context)
        btnGenScore = findViewById(R.id.btn_gen_score)
        btnImport = findViewById(R.id.btn_import)
        btnSave = findViewById(R.id.btn_save)
        btnPlay = findViewById(R.id.btn_play)
        btnStop = findViewById(R.id.btn_stop)
        btnExport = findViewById(R.id.btn_export)
        statusText = findViewById(R.id.status_text)

        loadInitialContent()

        btnGenScore.setOnClickListener { generateScore() }
        btnImport.setOnClickListener { importPicker.launch(arrayOf("*/*")) }
        btnSave.setOnClickListener { saveScore() }
        btnPlay.setOnClickListener { play() }
        btnStop.setOnClickListener { stopPlayback() }
        btnExport.setOnClickListener { export() }
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.main_menu, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == R.id.action_settings) {
            showSettingsDialog()
            return true
        }
        return super.onOptionsItemSelected(item)
    }

    // ── AI 设置 (OpenAI API 格式) ─────────────────────────────────
    private fun showSettingsDialog() {
        val view = layoutInflater.inflate(R.layout.dialog_settings, null)
        val etBase = view.findViewById<EditText>(R.id.et_base_url)
        val etKey = view.findViewById<EditText>(R.id.et_api_key)
        val etModel = view.findViewById<EditText>(R.id.et_model)
        val spinnerEffort = view.findViewById<Spinner>(R.id.spinner_reasoning_effort)
        etBase.setText(prefs.getString(KEY_BASE_URL, ""))
        etKey.setText(prefs.getString(KEY_API_KEY, ""))
        etModel.setText(prefs.getString(KEY_MODEL, DEFAULT_MODEL))

        // 思考强度: 默认("")/低/中/高 → reasoning_effort 请求参数
        val effortLabels = arrayOf(
            getString(R.string.effort_default),
            getString(R.string.effort_low),
            getString(R.string.effort_medium),
            getString(R.string.effort_high)
        )
        val effortValues = arrayOf("", "low", "medium", "high")
        spinnerEffort.adapter = ArrayAdapter(
            this, android.R.layout.simple_spinner_dropdown_item, effortLabels
        )
        val curEffort = prefs.getString(KEY_REASONING_EFFORT, "") ?: ""
        spinnerEffort.setSelection(effortValues.indexOf(curEffort).coerceAtLeast(0))

        // 按钮触发在线获取模型列表 (GET {base}/v1/models)
        val btnPick = view.findViewById<Button>(R.id.btn_model_pick)
        btnPick.setOnClickListener {
            btnPick.isEnabled = false
            btnPick.text = getString(R.string.model_loading)
            fetchModels(
                onResult = { models ->
                    btnPick.isEnabled = true
                    btnPick.text = getString(R.string.model_pick)
                    if (models.isEmpty()) {
                        toast(getString(R.string.error_models_empty))
                    } else {
                        AlertDialog.Builder(this)
                            .setTitle(R.string.settings_model)
                            .setItems(models.toTypedArray()) { _, which ->
                                etModel.setText(models[which])
                            }
                            .show()
                    }
                },
                onError = { msg ->
                    btnPick.isEnabled = true
                    btnPick.text = getString(R.string.model_pick)
                    toast(getString(R.string.error_ai_call, msg))
                }
            )
        }

        AlertDialog.Builder(this)
            .setTitle(R.string.menu_settings)
            .setView(view)
            .setPositiveButton(R.string.dialog_save) { _, _ ->
                prefs.edit()
                    .putString(KEY_BASE_URL, etBase.text.toString().trim())
                    .putString(KEY_API_KEY, etKey.text.toString().trim())
                    .putString(KEY_MODEL, etModel.text.toString().trim().ifEmpty { DEFAULT_MODEL })
                    .putString(KEY_REASONING_EFFORT, effortValues[spinnerEffort.selectedItemPosition])
                    .apply()
            }
            .setNegativeButton(R.string.dialog_cancel, null)
            .show()
    }

    // ── AI 生成乐谱 (流式输出) ────────────────────────────────────
    private fun generateScore() {
        val requirement = promptInput.text.toString().trim()
        if (requirement.isEmpty()) {
            toast(getString(R.string.error_prompt_empty))
            return
        }
        // 取当前乐谱作上下文 (在流式输出覆盖编辑框之前捕获)
        val contextContent = editor.text.toString().trim()
        val userPrompt = buildUserPrompt(requirement, chkIncludeContext.isChecked, contextContent)

        setGenerating(true, getString(R.string.status_generating_score))
        executor.execute {
            try {
                streamLlm(
                    SCORE_SYSTEM_PROMPT, userPrompt,
                    onDelta = { text -> mainHandler.post { editor.setText(text) } },
                    onDone = { full -> handleGenerated(full) }
                )
            } catch (e: Exception) {
                // 流式连接被中断 (如 "Software caused connection abort") 时, 回退一次性请求重试
                try {
                    handleGenerated(callOnce(SCORE_SYSTEM_PROMPT, userPrompt))
                } catch (e2: Exception) {
                    mainHandler.post {
                        setGenerating(false, getString(R.string.error_ai_call, e2.message ?: ""))
                    }
                }
            }
        }
    }

    /** 处理一次完整的生成结果: C++ 校验 + 写入编辑框 + 状态提示 */
    private fun handleGenerated(full: String) {
        if (full.isBlank()) {
            mainHandler.post {
                setGenerating(false, getString(R.string.error_ai_call, getString(R.string.error_empty)))
            }
            return
        }
        // 立即用 C++ 渲染校验格式 (第 2 行音色先剥掉)
        val stripped = normalizeNotesContent(full)
        val formatError = try {
            val timbre = parseTimbre(full)?.toDoubleArray()
                ?: MusicNative.getTimbreHarmonics("piano")
            MusicNative.renderPcm(stripped, timbre, SAMPLE_RATE)
            null
        } catch (e: Throwable) {
            e.message
        }
        mainHandler.post {
            editor.setText(full)
            if (formatError == null) {
                setGenerating(false, getString(R.string.status_score_ok))
            } else {
                setGenerating(false, getString(R.string.status_score_warning, formatError ?: ""))
            }
        }
    }

    /** 构造用户消息; 勾选时把当前乐谱作为上下文附在后面 */
    private fun buildUserPrompt(requirement: String, includeContext: Boolean, contextContent: String): String {
        return if (includeContext && contextContent.isNotEmpty()) {
            "$requirement\n\n以下是当前乐谱内容，可基于它续写、修改或优化，输出仍为完整 RCP（含头部、音色、音符）：\n$contextContent"
        } else {
            requirement
        }
    }

    /** base_url 规范化: 自动补 https:// 与 /v1 (用户填写的地址不带 v1) */
    private fun apiBase(): String {
        var base = prefs.getString(KEY_BASE_URL, "")?.trim().orEmpty().trimEnd('/')
        if (base.isNotEmpty() && !base.startsWith("http://") && !base.startsWith("https://")) {
            base = "https://$base"
        }
        return if (base.endsWith("/v1")) base else "$base/v1"
    }

    private fun apiKey(): String = prefs.getString(KEY_API_KEY, "")?.trim().orEmpty()

    /** 当前思考强度 (reasoning_effort); 返回 null 表示不发送该参数 */
    private fun reasoningEffort(): String? {
        val v = prefs.getString(KEY_REASONING_EFFORT, "")?.trim().orEmpty()
        return if (v.isEmpty()) null else v
    }

    /**
     * 流式调用 OpenAI 兼容接口 POST {base}/v1/chat/completions (stream: true)
     * 逐块回调 onDelta(累计文本), 结束后回调 onDone(清洗后的完整文本)
     */
    private fun streamLlm(
        systemPrompt: String,
        userPrompt: String,
        onDelta: (String) -> Unit,
        onDone: (String) -> Unit,
    ) {
        val base = apiBase()
        val key = apiKey()
        val model = prefs.getString(KEY_MODEL, DEFAULT_MODEL)?.trim().orEmpty()
        if (base.isEmpty() || key.isEmpty())
            throw IllegalStateException(getString(R.string.error_ai_settings))

        val conn = URL("$base/chat/completions").openConnection() as HttpURLConnection
        try {
            conn.requestMethod = "POST"
            conn.connectTimeout = 30_000
            conn.readTimeout = 120_000
            conn.setRequestProperty("Content-Type", "application/json")
            conn.setRequestProperty("Authorization", "Bearer $key")
            conn.setRequestProperty("Accept", "text/event-stream")

            val body = JSONObject().apply {
                put("model", model)
                put("temperature", 0.8)
                put("stream", true)
                reasoningEffort()?.let { put("reasoning_effort", it) }
                put("messages", JSONArray().apply {
                    put(JSONObject().put("role", "system").put("content", systemPrompt))
                    put(JSONObject().put("role", "user").put("content", userPrompt))
                })
            }.toString()
            conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }

            val code = conn.responseCode
            if (code !in 200..299) {
                val err = conn.errorStream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() } ?: ""
                throw IllegalStateException("HTTP $code: ${err.take(200)}")
            }

            val sb = StringBuilder()
            conn.inputStream.bufferedReader(Charsets.UTF_8).use { reader ->
                while (true) {
                    val line = reader.readLine() ?: break
                    val l = line.trim()
                    if (l.isEmpty() || !l.startsWith("data:")) continue
                    val data = l.removePrefix("data:").trim()
                    if (data == "[DONE]") break
                    val delta = try {
                        // 用类型判断而非 optString: JSON 里的 null 会被 optString 转成字符串 "null"
                        val choice = JSONObject(data)
                            .optJSONArray("choices")?.optJSONObject(0)
                        val content = choice
                            ?.optJSONObject("delta")
                            ?.opt("content")
                        if (content is String) content else null
                    } catch (_: Exception) {
                        null
                    }
                    if (!delta.isNullOrEmpty()) {
                        sb.append(delta)
                        onDelta(sb.toString())
                    }
                }
            }
            onDone(cleanLlmText(sb.toString()))
        } catch (e: UnknownHostException) {
            throw IllegalStateException(getString(R.string.error_dns, e.message ?: ""))
        } finally {
            conn.disconnect()
        }
    }

    /** 一次性 (非流式) 调用, 作为流式失败时的回退 */
    private fun callOnce(systemPrompt: String, userPrompt: String): String {
        val base = apiBase()
        val key = apiKey()
        val model = prefs.getString(KEY_MODEL, DEFAULT_MODEL)?.trim().orEmpty()
        if (base.isEmpty() || key.isEmpty())
            throw IllegalStateException(getString(R.string.error_ai_settings))

        val conn = URL("$base/chat/completions").openConnection() as HttpURLConnection
        try {
            conn.requestMethod = "POST"
            conn.connectTimeout = 30_000
            conn.readTimeout = 120_000
            conn.setRequestProperty("Content-Type", "application/json")
            conn.setRequestProperty("Authorization", "Bearer $key")

            val body = JSONObject().apply {
                put("model", model)
                put("temperature", 0.8)
                put("stream", false)
                reasoningEffort()?.let { put("reasoning_effort", it) }
                put("messages", JSONArray().apply {
                    put(JSONObject().put("role", "system").put("content", systemPrompt))
                    put(JSONObject().put("role", "user").put("content", userPrompt))
                })
            }.toString()
            conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }

            val code = conn.responseCode
            if (code !in 200..299) {
                val err = conn.errorStream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() } ?: ""
                throw IllegalStateException("HTTP $code: ${err.take(200)}")
            }
            val resp = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
            return cleanLlmText(
                JSONObject(resp)
                    .getJSONArray("choices")
                    .getJSONObject(0)
                    .getJSONObject("message")
                    .getString("content")
            )
        } catch (e: UnknownHostException) {
            throw IllegalStateException(getString(R.string.error_dns, e.message ?: ""))
        } finally {
            conn.disconnect()
        }
    }

    /** 在线获取模型列表: GET {base}/v1/models, 解析 data[].id */
    private fun fetchModels(onResult: (List<String>) -> Unit, onError: (String) -> Unit) {
        val base = apiBase()
        val key = apiKey()
        if (base.isEmpty() || key.isEmpty()) {
            onError(getString(R.string.error_ai_settings))
            return
        }
        executor.execute {
            try {
                val conn = URL("$base/models").openConnection() as HttpURLConnection
                try {
                    conn.requestMethod = "GET"
                    conn.connectTimeout = 30_000
                    conn.readTimeout = 30_000
                    conn.setRequestProperty("Authorization", "Bearer $key")

                    val code = conn.responseCode
                    if (code !in 200..299) {
                        val err = conn.errorStream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() } ?: ""
                        throw IllegalStateException("HTTP $code: ${err.take(200)}")
                    }
                    val resp = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
                    val data = JSONObject(resp).getJSONArray("data")
                    val models = (0 until data.length()).map {
                        data.getJSONObject(it).getString("id")
                    }
                    mainHandler.post { onResult(models) }
                } finally {
                    conn.disconnect()
                }
            } catch (e: UnknownHostException) {
                mainHandler.post { onError(getString(R.string.error_dns, e.message ?: "")) }
            } catch (e: Exception) {
                mainHandler.post { onError(e.message ?: "网络错误") }
            }
        }
    }

    /** 去掉 LLM 输出常见的 ``` 代码块围栏 */
    private fun cleanLlmText(raw: String): String {
        var s = raw.trim()
        if (s.startsWith("```")) {
            val newline = s.indexOf('\n')
            s = if (newline >= 0) s.substring(newline + 1) else ""
        }
        return s.trim().removeSuffix("```").trim()
    }

    /** 解析 "1,0.5,0.3" 形式的振幅列表; 非法时返回空列表 */
    private fun parseHarmonics(s: String): List<Double> {
        val out = mutableListOf<Double>()
        for (token in s.split(',')) {
            val v = token.trim().toDoubleOrNull() ?: return emptyList()
            if (v < 0.0) return emptyList()
            out.add(v.coerceAtMost(1.0))
        }
        return out
    }

    private fun setGenerating(generating: Boolean, status: String) {
        btnGenScore.isEnabled = !generating
        statusText.text = status
    }

    /** 当前音色: 优先取编辑器第 2 行, 缺失时回退内置钢琴 */
    private fun currentHarmonics(): DoubleArray {
        val fromEditor = parseTimbre(editor.text.toString())
        if (fromEditor != null) return fromEditor.toDoubleArray()
        return MusicNative.getTimbreHarmonics("piano")
    }

    /** 从编辑内容提取第 2 行音色 (谐波振幅列表); 缺失/无效返回 null */
    private fun parseTimbre(content: String): List<Double>? {
        val nonEmpty = content.lineSequence()
            .map { it.trim() }
            .filter { it.isNotEmpty() }
            .toList()
        if (nonEmpty.size < 2) return null
        val harmonics = parseHarmonics(nonEmpty[1])
        return if (harmonics.isEmpty()) null else harmonics
    }

    /** 去掉编辑内容的第 2 行 (音色行), 返回可交给 C++ 渲染的内容 (头部 + 音符) */
    private fun stripTimbreLine(content: String): String {
        val lines = content.lineSequence().toList()
        if (lines.size < 2) return content
        return (lines[0] + "\n" + lines.drop(2).joinToString("\n")).trim()
    }

    /** 若第 2 行是音色行则剥掉, 否则原样返回 (兼容无音色行的旧格式) */
    private fun normalizeNotesContent(content: String): String {
        return if (parseTimbre(content) != null) stripTimbreLine(content) else content
    }

    // ── 导入 RCP 文件 ────────────────────────────────────────────
    private fun importRcp(uri: Uri) {
        statusText.text = getString(R.string.status_importing)
        executor.execute {
            try {
                val text = contentResolver
                    .openInputStream(uri)?.bufferedReader(Charsets.UTF_8)?.use { it.readText() }
                if (text == null) {
                    mainHandler.post { toast(getString(R.string.error_import_fail)) }
                    return@execute
                }
                val name = queryDisplayName(uri)
                mainHandler.post {
                    editor.setText(if (text.startsWith('\uFEFF')) text.substring(1) else text)
                    statusText.text = getString(R.string.status_imported, name)
                }
            } catch (e: Exception) {
                mainHandler.post { toast(getString(R.string.error_import_fail)) }
            }
        }
    }

    private fun queryDisplayName(uri: Uri): String {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { c ->
                if (c.moveToFirst()) {
                    val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0) c.getString(idx)?.let { return it }
                }
            }
        return uri.lastPathSegment ?: "rcp"
    }

    // ── 播放 ──────────────────────────────────────────────────────
    private fun play() {
        if (playing) return
        val content = editor.text.toString()
        if (content.isBlank()) {
            toast("请输入乐谱内容")
            return
        }
        val timbreList = parseTimbre(content)
        val harmonics = timbreList?.toDoubleArray() ?: MusicNative.getTimbreHarmonics("piano")
        val stripped = normalizeNotesContent(content)

        playing = true
        btnPlay.isEnabled = false
        btnStop.isEnabled = true
        statusText.text = if (timbreList != null) {
            getString(R.string.status_timbre_info, timbreList.joinToString(","))
        } else {
            getString(R.string.status_timbre_fallback)
        }

        executor.execute {
            try {
                val pcm = MusicNative.renderPcm(stripped, harmonics, SAMPLE_RATE)
                if (pcm.isEmpty()) {
                    mainHandler.post { toast(getString(R.string.error_no_audio)) }
                    return@execute
                }
                val seconds = pcm.size / 2 / SAMPLE_RATE
                mainHandler.post {
                    statusText.text = getString(R.string.status_playing, seconds)
                }
                playPcm(pcm)
            } catch (e: Throwable) {
                mainHandler.post {
                    toast(getString(R.string.error_render, e.message))
                }
            } finally {
                playing = false
                mainHandler.post {
                    btnPlay.isEnabled = true
                    btnStop.isEnabled = false
                    if (track == null) {
                        statusText.text = getString(R.string.status_ready)
                    }
                }
            }
        }
    }

    private fun playPcm(pcm: ByteArray) {
        val minBuf = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        val bufSize = maxOf(minBuf, 8192)
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build()
            )
            .setBufferSizeInBytes(bufSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        track = t

        try {
            t.play()
            var offset = 0
            while (offset < pcm.size && playing) {
                val written = t.write(pcm, offset, minOf(bufSize, pcm.size - offset))
                if (written < 0) break
                offset += written
            }
        } finally {
            try {
                t.stop()
            } catch (_: IllegalStateException) {
            }
            t.release()
            track = null
        }
    }

    private fun stopPlayback() {
        playing = false
        statusText.text = getString(R.string.status_ready)
    }

    // ── 导出 WAV ──────────────────────────────────────────────────
    private fun export() {
        val content = editor.text.toString()
        if (content.isBlank()) {
            toast(getString(R.string.error_empty))
            return
        }
        val harmonics = currentHarmonics()
        val stripped = normalizeNotesContent(content)
        val timbreName = "custom"

        btnExport.isEnabled = false
        statusText.text = getString(R.string.status_exporting)

        executor.execute {
            try {
                val wav = MusicNative.renderWav(stripped, harmonics, SAMPLE_RATE)
                val name = "music_${timbreName}_${System.currentTimeMillis()}.wav"
                val uri = saveToDownloads(name, wav)
                mainHandler.post {
                    if (uri != null) {
                        statusText.text = getString(R.string.status_exported, name)
                        toast(getString(R.string.status_exported, name))
                    } else {
                        statusText.text = getString(R.string.error_export_fail)
                        toast(getString(R.string.error_export_fail))
                    }
                }
            } catch (e: Throwable) {
                mainHandler.post {
                    statusText.text = getString(R.string.error_export_fail)
                    toast(getString(R.string.error_render, e.message))
                }
            } finally {
                mainHandler.post { btnExport.isEnabled = true }
            }
        }
    }

    private fun saveToDownloads(fileName: String, bytes: ByteArray): Uri? {
        val values = ContentValues().apply {
            put(MediaStore.MediaColumns.DISPLAY_NAME, fileName)
            put(MediaStore.MediaColumns.MIME_TYPE, "audio/wav")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
            }
        }
        val collection =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                MediaStore.Downloads.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY)
            } else {
                MediaStore.Files.getContentUri("external")
            }
        val uri = contentResolver.insert(collection, values) ?: return null
        val ok = contentResolver.openOutputStream(uri)?.use { out ->
            out.write(bytes)
            true
        } ?: run {
            contentResolver.delete(uri, null, null)
            false
        }
        return if (ok) uri else null
    }

    // ── 工具 ──────────────────────────────────────────────────────
    /** 保存当前乐谱到内部存储 (无权限要求) */
    private fun saveScore() {
        val content = editor.text.toString()
        if (content.isBlank()) {
            toast(getString(R.string.error_empty))
            return
        }
        try {
            savedScoreFile.parentFile?.mkdirs()
            savedScoreFile.writeText(content)
            statusText.text = getString(R.string.status_saved, savedScoreFile.absolutePath)
            toast(getString(R.string.status_saved, savedScoreFile.absolutePath))
        } catch (e: Exception) {
            statusText.text = getString(R.string.error_save_fail)
            toast(getString(R.string.error_save_fail))
        }
    }

    /** 启动时加载: 优先内部存储的乐谱, 否则示例谱 */
    private fun loadInitialContent() {
        if (editor.text.isNotBlank()) return
        val saved = savedScoreFile
        if (saved.exists()) {
            try {
                editor.setText(saved.readText())
                return
            } catch (_: Exception) {
            }
        }
        val text = try {
            assets.open("sample.rcp").bufferedReader().use { it.readText() }
        } catch (_: Exception) {
            ""
        }
        if (text.isNotBlank()) editor.setText(text)
    }

    private fun toast(msg: String) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }

    override fun onDestroy() {
        super.onDestroy()
        playing = false
        executor.shutdownNow()
        try {
            track?.stop()
        } catch (_: IllegalStateException) {
        }
        track?.release()
        track = null
    }
}
