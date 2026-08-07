package com.music.editor

/**
 * C++ 后端 JNI 桥接 (music-native.so)
 *
 * 对应桌面版 music_editor 的 core + save 工具:
 *  - 简谱解析: core/note_parser
 *  - 泛音合成: core/tone_gen
 *  - WAV 编码: core/wav_writer
 */
object MusicNative {

    init {
        System.loadLibrary("music-native")
    }

    /** 可用内置音色名称列表 (piano / violin / flute) */
    external fun getTimbres(): Array<String>

    /** 内置音色各倍频振幅, 第 0 个为基频 (double[]) */
    external fun getTimbreHarmonics(name: String): DoubleArray

    /**
     * 将 RCP 文本渲染为 16-bit PCM (单声道, 小端).
     * harmonics: 各倍频振幅, 第 0 个为基频, 不定长.
     * 供 AudioTrack 直接播放.
     */
    external fun renderPcm(content: String, harmonics: DoubleArray, sampleRate: Int): ByteArray

    /** 将 RCP 文本渲染为完整 WAV 文件字节, 供导出/分享. */
    external fun renderWav(content: String, harmonics: DoubleArray, sampleRate: Int): ByteArray

    /**
     * 带持续比例渲染为 16-bit PCM.
     * sustainLine: 第三行持续比例, 空格分隔, 每项对应一个倍频,
     * 形如 "0-1 0.3-0.5 {0.1-0.3,0.5-0.7}", 可传空串表示全程恒有.
     */
    external fun renderPcmSustain(content: String, harmonics: DoubleArray, sustainLine: String, sampleRate: Int): ByteArray

    /** 带持续比例渲染为完整 WAV 文件字节, 供导出/分享. */
    external fun renderWavSustain(content: String, harmonics: DoubleArray, sustainLine: String, sampleRate: Int): ByteArray
}
