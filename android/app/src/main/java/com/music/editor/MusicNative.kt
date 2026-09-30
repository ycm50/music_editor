package com.music.editor

/**
 * C++ 后端 JNI 桥接 (music-native.so)
 *
 * 对应桌面版 music_editor 的 core + save 工具:
 *  - 简谱解析: core/note_parser (parse_rcp)
 *  - 物理合成: core/tone_gen  (render_document)
 *  - WAV 编码: core/wav_writer
 *
 * 解析与合成完全在 C++ 侧完成, Kotlin 只负责传文本与播放, 与桌面端行为一致。
 */
object MusicNative {

    init {
        System.loadLibrary("music-native")
    }

    /** 可用物理乐器名称列表 (33 种: piano/guzheng/erhu/dizi/bells/...) */
    external fun getInstruments(): Array<String>

    /** 乐器预设目录: 每项 "名称|分类|说明" (供插入面板的预设列表) */
    external fun getPresetCatalog(): Array<String>

    /**
     * 由勾选项构造一个 RCP 音符 token (与桌面端共用一个构造器)。
     *
     * spec 形如 "degree=1;acc=0;oct=0;den=4;dot=0;tup=0;vel=mf;art=0;rep=1;voice=0;
     * chord=3.04,5.04;pos=0.125;damper=0.08" — 未给出的键用默认值。
     */
    external fun buildNoteToken(spec: String): String

    /** "@timbre <名称>" 一行 */
    external fun buildTimbreLine(name: String): String

    /** 预设的全部物理参数展开成 "@acoustic ..." 一行 (可直接替换文件内乐器) */
    external fun buildAcousticLine(name: String): String

    /** 旧式裸谐波音色名称列表 (piano / violin / flute) */
    external fun getTimbres(): Array<String>

    /** 音色各倍频振幅, 第 0 个为基频 (double[]); 物理乐器会返回其推导频谱 */
    external fun getTimbreHarmonics(name: String): DoubleArray

    /**
     * 将 RCP 文本渲染为 16-bit PCM (小端)。
     *
     * harmonics: 回退用的各倍频振幅 (文件内 @timbre/@acoustic 优先), 可传空数组。
     * stereo:    true = 左右交错立体声 (含混响), false = 单声道。
     * 供 AudioTrack 直接播放。
     */
    external fun renderPcm(content: String, harmonics: DoubleArray, sampleRate: Int, stereo: Boolean): ByteArray

    /** 将 RCP 文本渲染为完整 WAV 文件字节, 供导出/分享。 */
    external fun renderWav(content: String, harmonics: DoubleArray, sampleRate: Int, stereo: Boolean): ByteArray

    /**
     * 兼容旧签名 (单声道, 无持续比例)。
     * sustainLine 参数已废弃 — 新格式请把持续比例/物理模型直接写在内容里。
     */
    external fun renderPcmSustain(content: String, harmonics: DoubleArray, sustainLine: String, sampleRate: Int, stereo: Boolean): ByteArray

    /** 兼容旧签名 (单声道)。 */
    external fun renderWavSustain(content: String, harmonics: DoubleArray, sustainLine: String, sampleRate: Int, stereo: Boolean): ByteArray

    /** 乐理/声学体检报告 (时长、音域、直流、混叠、拍号对齐、提示) */
    external fun analyze(content: String, sampleRate: Int): String

    /**
     * 生成乐谱用的系统提示词。
     * 正文唯一来源是 core/ai_prompt.cpp, 与桌面端共用同一份文本, 不会两端漂移。
     */
    external fun getScorePrompt(): String

    /** 提示词里那段示例乐谱 (可直接填入编辑器试听) */
    external fun getScorePromptExample(): String
}
