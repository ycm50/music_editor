package com.music.editor

import android.app.AlertDialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentValues
import android.content.Context
import android.graphics.Typeface
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
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
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

        // 生成谱: 只输出 RCP 文本本身。
        // 提示词正文唯一来源是 core/ai_prompt.cpp (经 JNI 的 getScorePrompt 取回),
        // 与桌面端共用同一份文本, 避免两端各写一份而漂移。
        private fun scoreSystemPrompt(): String =
            runCatching { MusicNative.getScorePrompt() }.getOrNull().orEmpty()
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
        when (item.itemId) {
            R.id.action_insert -> {
                showInsertDialog()
                return true
            }
            R.id.action_settings -> {
                showSettingsDialog()
                return true
            }
            R.id.action_copy_prompt -> {
                copySystemPrompt()
                return true
            }
        }
        return super.onOptionsItemSelected(item)
    }

    /** 复制 AI 生成提示词 (来自 core/ai_prompt.cpp) 到剪贴板 */
    private fun copySystemPrompt() {
        val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("RCP 提示词", scoreSystemPrompt()))
        toast(getString(R.string.prompt_copied))
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

    // ── 插入音符 / 乐器预设 (安卓端对等桌面端"插入面板") ──────────

    /** 打开插入对话框: 勾选 + 填参数, token 一律由 C++ core/note_builder 生成 */
    private fun showInsertDialog() {
        val pad = resources.getDimensionPixelSize(R.dimen.screen_padding)

        val container = LinearLayout(this)
        container.orientation = LinearLayout.VERTICAL
        container.setPadding(pad, pad, pad, pad)

        // ── 音高 ──
        val degreeValues = listOf(1, 2, 3, 4, 5, 6, 7, 0)   // 末项 = 休止
        val spDegree = Spinner(this)
        bindSpinner(spDegree, resources.getStringArray(R.array.insert_degree_items).toList())
        spDegree.setSelection(0)

        val accValues = listOf(0, 1, -1, 2, -2)
        val spAcc = Spinner(this)
        bindSpinner(spAcc, resources.getStringArray(R.array.insert_accidental_items).toList())

        val octValues = (0..8).map { it - 4 }
        val octLabels = octValues.map { if (it > 0) "+$it" else "$it" }
        val spOct = Spinner(this)
        bindSpinner(spOct, octLabels)
        spOct.setSelection(4)   // 0 = 中音

        // ── 时值 ──
        val denValues = listOf(1, 2, 4, 8, 16, 32)
        val spDen = Spinner(this)
        bindSpinner(spDen, resources.getStringArray(R.array.insert_denominator_items).toList())
        spDen.setSelection(2)   // 四分音符 = 1 拍

        val chkDotted = CheckBox(this)
        chkDotted.text = getString(R.string.insert_dotted)

        val tupValues = listOf(0, 3, 5)
        val spTup = Spinner(this)
        bindSpinner(spTup, resources.getStringArray(R.array.insert_tuplet_items).toList())

        // ── 力度 / 演奏法 ──
        val velNames = listOf("ppp", "pp", "p", "mp", "mf", "f", "ff", "fff")
        val spVel = Spinner(this)
        bindSpinner(spVel, velNames)
        spVel.setSelection(6)   // ff: 与 C++ NoteSpec 的默认力度一致

        val artValues = listOf(0, 1, 2)
        val spArt = Spinner(this)
        bindSpinner(spArt, resources.getStringArray(R.array.insert_articulation_items).toList())

        // ── 连音 / 重复 / 声部 / 和弦 ──
        val chkTie = CheckBox(this)
        chkTie.text = getString(R.string.insert_tie)

        val etRepeat = EditText(this)
        etRepeat.inputType = InputType.TYPE_CLASS_NUMBER
        etRepeat.setText("1")

        val etVoice = EditText(this)
        etVoice.inputType = InputType.TYPE_CLASS_NUMBER
        etVoice.setText("0")

        val etChord = EditText(this)
        etChord.inputType = InputType.TYPE_CLASS_TEXT
        etChord.setHint(R.string.insert_chord_hint)

        // ── 逐音物理覆盖 (勾选后才写进 spec) ──
        val ovRows = arrayListOf<Pair<CheckBox, EditText>>()
        fun addOvRow(label: String, defValue: String) {
            val row = LinearLayout(this)
            row.orientation = LinearLayout.HORIZONTAL
            val cb = CheckBox(this)
            cb.text = label
            val et = EditText(this)
            et.inputType = InputType.TYPE_CLASS_NUMBER or
                InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
            et.setText(defValue)
            et.isEnabled = false
            row.addView(cb)
            val lp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            row.addView(et, lp)
            container.addView(row)
            ovRows.add(Pair(cb, et))
        }

        val tvOvHead = TextView(this)
        tvOvHead.text = getString(R.string.insert_override_head)

        // ── 预览 ──
        val tvPreview = TextView(this)
        tvPreview.text = getString(R.string.insert_preview)
        val preview = TextView(this)
        preview.typeface = Typeface.MONOSPACE

        // ── 乐器预设入口 ──
        val btnPreset = Button(this)
        btnPreset.text = getString(R.string.insert_preset_entry)

        // ── 组装: 单行 = 标签 + 控件 ──
        fun addRow(label: String, child: View) {
            val row = LinearLayout(this)
            row.orientation = LinearLayout.HORIZONTAL
            val tv = TextView(this)
            tv.text = label
            row.addView(tv)
            val lp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            row.addView(child, lp)
            container.addView(row)
        }
        addRow(getString(R.string.insert_degree), spDegree)
        addRow(getString(R.string.insert_accidental), spAcc)
        addRow(getString(R.string.insert_octave), spOct)
        addRow(getString(R.string.insert_denominator), spDen)
        addRow(getString(R.string.insert_tuplet), spTup)
        addRow(getString(R.string.insert_velocity), spVel)
        addRow(getString(R.string.insert_articulation), spArt)
        container.addView(chkDotted)
        addRow(getString(R.string.insert_repeat), etRepeat)
        addRow(getString(R.string.insert_voice), etVoice)
        addRow(getString(R.string.insert_chord), etChord)
        container.addView(chkTie)
        container.addView(tvOvHead)
        addOvRow(getString(R.string.insert_ov_atk), "0.010")
        addOvRow(getString(R.string.insert_ov_dec), "1.000")
        addOvRow(getString(R.string.insert_ov_sus), "0.50")
        addOvRow(getString(R.string.insert_ov_br), "0.70")
        addOvRow(getString(R.string.insert_ov_b), "0.0002")
        addOvRow(getString(R.string.insert_ov_pos), "0.125")
        addOvRow(getString(R.string.insert_ov_damper), "0.080")
        container.addView(tvPreview)
        container.addView(preview)
        container.addView(btnPreset)

        // ── spec 组装 (未给出的键由 C++ 用默认值) ──
        fun pick(values: List<Int>, spinner: Spinner): Int {
            val p = spinner.selectedItemPosition
            return if (p >= 0 && p < values.size) values[p] else values[0]
        }
        fun readInt(et: EditText, def: Int): Int {
            val v = et.text.toString().trim()
            return if (v.isEmpty()) def else (v.toIntOrNull() ?: def)
        }
        fun buildSpec(): String {
            val sb = StringBuilder()
            sb.append("degree=").append(pick(degreeValues, spDegree))
            sb.append(";acc=").append(pick(accValues, spAcc))
            sb.append(";oct=").append(pick(octValues, spOct))
            sb.append(";den=").append(pick(denValues, spDen))
            sb.append(";dot=").append(if (chkDotted.isChecked) 1 else 0)
            sb.append(";tup=").append(pick(tupValues, spTup))
            sb.append(";tie=").append(if (chkTie.isChecked) 1 else 0)
            sb.append(";art=").append(pick(artValues, spArt))
            sb.append(";rep=").append(readInt(etRepeat, 1).coerceIn(1, 32))
            sb.append(";voice=").append(readInt(etVoice, 0).coerceIn(0, 8))
            sb.append(";vel=").append(velNames[spVel.selectedItemPosition.coerceIn(0, velNames.size - 1)])
            sb.append(";chord=").append(etChord.text.toString().trim())
            // 逐音物理覆盖: 勾选且填了数值才写入
            val ovKeys = listOf("atk", "dec", "sus", "br", "b", "pos", "damper")
            for (i in ovRows.indices) {
                if (i >= ovKeys.size) break
                val cb = ovRows[i].first
                val et = ovRows[i].second
                if (!cb.isChecked) continue
                val v = et.text.toString().trim()
                if (v.isEmpty()) continue
                sb.append(";").append(ovKeys[i]).append("=").append(v)
            }
            return sb.toString()
        }
        fun refreshPreview() {
            val token = try {
                MusicNative.buildNoteToken(buildSpec())
            } catch (e: Throwable) {
                getString(R.string.insert_preview_failed, e.message ?: "")
            }
            preview.text = token
        }
        fun currentToken(): String {
            return try {
                MusicNative.buildNoteToken(buildSpec())
            } catch (e: Throwable) {
                toast(getString(R.string.menu_insert_failed, e.message ?: ""))
                ""
            }
        }

        // ── 任何控件变化都刷新预览 ──
        val watcher = object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: Editable?) {
                refreshPreview()
            }
        }
        etRepeat.addTextChangedListener(watcher)
        etVoice.addTextChangedListener(watcher)
        etChord.addTextChangedListener(watcher)
        for (r in ovRows) {
            val cb = r.first
            val et = r.second
            cb.setOnCheckedChangeListener { _, checked ->
                et.isEnabled = checked
                refreshPreview()
            }
            et.addTextChangedListener(watcher)
        }
        val onSpinner = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                refreshPreview()
            }
            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
        spDegree.setOnItemSelectedListener(onSpinner)
        spAcc.setOnItemSelectedListener(onSpinner)
        spOct.setOnItemSelectedListener(onSpinner)
        spDen.setOnItemSelectedListener(onSpinner)
        spTup.setOnItemSelectedListener(onSpinner)
        spVel.setOnItemSelectedListener(onSpinner)
        spArt.setOnItemSelectedListener(onSpinner)
        chkDotted.setOnCheckedChangeListener { _, _ -> refreshPreview() }
        chkTie.setOnCheckedChangeListener { _, _ -> refreshPreview() }

        val scroll = ScrollView(this)
        scroll.addView(container)

        val builder = AlertDialog.Builder(this)
            .setTitle(R.string.insert_title)
            .setView(scroll)
            .setPositiveButton(R.string.insert_to_cursor) { _, _ ->
                val token = currentToken()
                if (token.isEmpty()) {
                    toast(getString(R.string.insert_note_empty))
                } else {
                    insertAtCursor(token)
                }
            }
            .setNeutralButton(R.string.insert_append_end) { _, _ ->
                val token = currentToken()
                if (token.isEmpty()) {
                    toast(getString(R.string.insert_note_empty))
                } else {
                    appendToEnd(token)
                }
            }
            .setNegativeButton(R.string.dialog_cancel, null)
        val dlg = builder.create()
        btnPreset.setOnClickListener {
            dlg.dismiss()
            showPresetDialog()
        }
        dlg.show()
        refreshPreview()
    }

    /** 给 Spinner 装上字符串适配器 (与 showSettingsDialog 同一风格) */
    private fun bindSpinner(spinner: Spinner, items: List<String>) {
        spinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, items)
    }

    /** 在编辑器光标处插入 token, 必要时前后补空格 (与桌面端 insert_text 一致) */
    private fun insertAtCursor(token: String) {
        if (token.isEmpty()) return
        val editable = editor.text
        val start = editor.selectionStart.coerceIn(0, editable.length)
        val end = editor.selectionEnd.coerceIn(0, editable.length)
        val from = minOf(start, end)
        val to = maxOf(start, end)
        val before = editable.subSequence(0, from).toString()
        val after = editable.subSequence(to, editable.length).toString()
        val needPre = before.isNotEmpty() && !before.endsWith(" ") &&
            !before.endsWith("\n") && !before.endsWith("\t")
        val needPost = after.isNotEmpty() && !after.startsWith(" ") &&
            !after.startsWith("\n") && !after.startsWith("\t")
        val text = (if (needPre) " " else "") + token + (if (needPost) " " else "")
        editable.replace(from, to, text)
        editor.setSelection(from + text.length)
    }

    /** 追加到编辑器文末 (与已有内容之间补一个换行) */
    private fun appendToEnd(token: String) {
        if (token.isEmpty()) return
        val editable = editor.text
        val at = editable.length
        val needNewline = at > 0 && !editable.toString().endsWith("\n")
        val text = (if (needNewline) "\n" else "") + token
        editable.replace(at, at, text)
        editor.setSelection(at + text.length)
    }

    /** 乐器预设列表: 每项 "名称|分类|说明" (来自 C++ 内置乐器目录) */
    private fun showPresetDialog() {
        val catalog = try {
            MusicNative.getPresetCatalog()
        } catch (e: Throwable) {
            toast(getString(R.string.menu_insert_failed, e.message ?: ""))
            return
        }
        if (catalog.isEmpty()) {
            toast(getString(R.string.preset_empty))
            return
        }
        val items = catalog.map { presetLabel(it) }
        AlertDialog.Builder(this)
            .setTitle(R.string.preset_title)
            .setItems(items.toTypedArray()) { _, which ->
                if (which in catalog.indices) onPresetPicked(catalog[which])
            }
            .setNegativeButton(R.string.dialog_cancel, null)
            .show()
    }

    /** "名称|分类|说明" → "分类 · 名称 · 说明" (缺项自动跳过) */
    private fun presetLabel(entry: String): String {
        val parts = entry.split('|')
        val name = parts.getOrNull(0).orEmpty()
        val category = parts.getOrNull(1).orEmpty()
        val desc = parts.getOrNull(2).orEmpty()
        val sb = StringBuilder()
        if (category.isNotEmpty()) sb.append(category).append(" · ")
        sb.append(name)
        if (desc.isNotEmpty()) sb.append(" · ").append(desc)
        return sb.toString()
    }

    /** 选中一个预设: 可插入 @timbre 行, 或插入完整 @acoustic 行 */
    private fun onPresetPicked(entry: String) {
        val name = entry.split('|').getOrNull(0).orEmpty()
        if (name.isEmpty()) return
        AlertDialog.Builder(this)
            .setTitle(presetLabel(entry))
            .setPositiveButton(R.string.preset_insert_timbre) { _, _ -> applyTimbrePreset(name) }
            .setNeutralButton(R.string.preset_insert_acoustic) { _, _ -> applyAcousticPreset(name) }
            .setNegativeButton(R.string.dialog_cancel, null)
            .show()
    }

    /** 插入 "@timbre 名称" 行 (已有同类行则替换) */
    private fun applyTimbrePreset(name: String) {
        val line = try {
            MusicNative.buildTimbreLine(name)
        } catch (e: Throwable) {
            toast(getString(R.string.menu_insert_failed, e.message ?: ""))
            return
        }
        if (line.isBlank()) {
            toast(getString(R.string.menu_insert_failed, name))
            return
        }
        // 旧别名 @instrument 也算同类行
        applyDirectiveLine(listOf("@timbre", "@instrument"), line)
    }

    /** 插入完整 "@acoustic ..." 行 (已有同类行则替换) */
    private fun applyAcousticPreset(name: String) {
        val line = try {
            MusicNative.buildAcousticLine(name)
        } catch (e: Throwable) {
            toast(getString(R.string.menu_insert_failed, e.message ?: ""))
            return
        }
        if (line.isBlank()) {
            toast(getString(R.string.preset_no_acoustic, name))
            return
        }
        applyDirectiveLine(listOf("@acoustic"), line)
    }

    /** 把以 prefixes 之一开头的行替换为 line; 没有则插到头部 ("BPM,ref,beat") 之后 */
    private fun applyDirectiveLine(prefixes: List<String>, line: String) {
        val lines = editor.text.toString().split('\n').toMutableList()
        var replaced = -1
        for (i in lines.indices) {
            val t = lines[i].trim()
            if (prefixes.any { t.startsWith(it) }) {
                lines[i] = line
                replaced = i
                break
            }
        }
        if (replaced < 0) {
            var pos = 0
            val header = Regex("""^\s*\d+(\.\d+)?\s*,""")
            for (i in lines.indices) {
                if (header.containsMatchIn(lines[i])) {
                    pos = i + 1
                    break
                }
            }
            lines.add(pos, line)
        }
        editor.setText(lines.joinToString("\n"))
        editor.setSelection(editor.text.length)
        toast(getString(R.string.preset_inserted, line))
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
        val systemPrompt = scoreSystemPrompt()
        if (systemPrompt.isBlank()) {
            toast(getString(R.string.error_ai_call, "系统提示词加载失败"))
            return
        }

        setGenerating(true, getString(R.string.status_generating_score))
        executor.execute {
            try {
                streamLlm(
                    systemPrompt, userPrompt,
                    onDelta = { text -> mainHandler.post { editor.setText(text) } },
                    onDone = { full -> handleGenerated(full) }
                )
            } catch (e: Exception) {
                // 流式连接被中断 (如 "Software caused connection abort") 时, 回退一次性请求重试
                try {
                    handleGenerated(callOnce(systemPrompt, userPrompt))
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
        // 立即用 C++ 走一遍解析+合成+体检 (与桌面端同一套 core)
        val formatError = try {
            val report = MusicNative.analyze(full, SAMPLE_RATE)
            if (report.contains("! 未生成音频")) report else null
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

    /** 从内容里读取 @timbre 行的乐器名 (供导出文件名/状态显示)；无则返回 null */
    private fun parseTimbreName(content: String): String? {
        for (raw in content.lineSequence()) {
            val l = raw.trim()
            if (!l.startsWith("@timbre") && !l.startsWith("@instrument")) continue
            val name = l.substringAfter(' ', "").trim().lowercase()
            if (name.isNotEmpty()) return name
        }
        return null
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

    /** 单个持续比例条目是否合法: "0-1" / "0.3-0.5" / "{0.1-0.3,0.5-0.7}" */
    private fun isSustainToken(tok: String): Boolean {
        val regionRe = Regex("""\d+(\.\d+)?-\d+(\.\d+)?>?""")
        if (tok.startsWith("{")) {
            if (!tok.endsWith("}")) return false
            val inner = tok.substring(1, tok.length - 1)
            if (inner.isBlank()) return false
            return inner.split(',').all { it.trim().matches(regionRe) }
        }
        return tok.matches(regionRe)
    }

    /** 提取第 3 行持续比例行; 不存在或非持续比例格式返回 null */
    private fun parseSustainLine(content: String): String? {
        val nonEmpty = content.lineSequence()
            .map { it.trim() }
            .filter { it.isNotEmpty() }
            .toList()
        if (nonEmpty.size < 3) return null
        if (parseTimbre(content) == null) return null
        val line3 = nonEmpty[2]
        val tokens = line3.split('!').map { it.trim() }.filter { it.isNotEmpty() }
        if (tokens.isEmpty()) return null
        if (!tokens.all { isSustainToken(it) }) return null
        return line3
    }

    /** 去掉编辑内容的第 2 行 (音色行), 返回可交给 C++ 渲染的内容 (头部 + 音符) */
    private fun stripTimbreLine(content: String): String {
        val lines = content.lineSequence().toList()
        if (lines.size < 2) return content
        return (lines[0] + "\n" + lines.drop(2).joinToString("\n")).trim()
    }

    /** 去掉编辑内容的第 2、3 行 (音色行 + 持续比例行), 返回可渲染内容 (头部 + 音符) */
    private fun stripTimbreAndSustainLine(content: String): String {
        val lines = content.lineSequence().toList()
        if (lines.size < 3) return content
        return (lines[0] + "\n" + lines.drop(3).joinToString("\n")).trim()
    }

    /** 若第 2 行是音色行则剥掉 (第 3 行为持续比例时一并剥掉), 否则原样返回 */
    private fun normalizeNotesContent(content: String): String {
        if (parseTimbre(content) == null) return content
        return if (parseSustainLine(content) != null) stripTimbreAndSustainLine(content)
               else stripTimbreLine(content)
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
                // 解析与合成全在 C++ 侧: 内容原样传入, 文件内 @timbre/@acoustic 优先
                val pcm = MusicNative.renderPcm(content, harmonics, SAMPLE_RATE, true)
                if (pcm.isEmpty()) {
                    mainHandler.post { toast(getString(R.string.error_no_audio)) }
                    return@execute
                }
                val seconds = pcm.size / 2 / 2 / SAMPLE_RATE   // 2 字节/样本 × 2 声道
                mainHandler.post {
                    statusText.text = getString(R.string.status_playing, seconds)
                }
                playPcm(pcm, stereo = true)
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

    private fun playPcm(pcm: ByteArray, stereo: Boolean) {
        val channelMask = if (stereo) AudioFormat.CHANNEL_OUT_STEREO
                          else AudioFormat.CHANNEL_OUT_MONO
        val minBuf = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            channelMask,
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
                    .setChannelMask(channelMask)
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
        val timbreName = parseTimbreName(content) ?: "custom"

        btnExport.isEnabled = false
        statusText.text = getString(R.string.status_exporting)

        executor.execute {
            try {
                val wav = MusicNative.renderWav(content, harmonics, SAMPLE_RATE, true)
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
