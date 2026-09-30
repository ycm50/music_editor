#ifndef INSERTPAGE_H
#define INSERTPAGE_H

#include "note_parser.h"
#include "note_builder.h"

#include <QWidget>
#include <QString>

class QTextEdit;
class QVBoxLayout;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLineEdit;
class QListWidget;
class QLabel;

/**
 * 插入面板: 用勾选框 + 参数的方式构造音符与乐器设置
 *
 *  页 1「音符插入」: 音级/变音/八度/拍长/附点/连音/力度/演奏法/和弦/逐音物理覆盖,
 *                    实时预览 token, 一键插入到编辑器光标处。
 *  页 2「乐器预设」: 33 种物理乐器预设 (按分类) + 全部物理参数可勾选/微调,
 *                    可插入 @timbre 行或完整的 @acoustic 行。
 *
 * token 文本一律由 core/note_builder (两端共用) 生成, 面板只负责收集勾选与参数。
 */
class InsertPage : public QWidget
{
    Q_OBJECT

public:
    explicit InsertPage(QTextEdit* target, QWidget* parent = nullptr);

    /// 在编辑器光标处插入文本 (自动补空格)
    void insert_text(const QString& text);

private:
    void build_note_tab();
    void build_instrument_tab();
    void refresh_preview();
    void update_preset_view();
    void on_add_chord_member();
    void on_clear_chord();
    void on_insert_note();
    void on_append_note();
    void on_copy_note();
    void on_apply_timbre();
    void on_insert_acoustic();
    void on_copy_acoustic();
    void load_preset_into_fields(const AcousticParams& p);
    AcousticParams collect_params() const;
    NoteSpec current_spec() const;

    /// 把以 @key 开头的行替换掉, 没有就插到头部之后
    void apply_directive_line(const QString& key, const QString& line);

    QTextEdit* target_ = nullptr;
    QVBoxLayout* note_layout_ = nullptr;
    QVBoxLayout* inst_layout_ = nullptr;

    // ── 页 1 ────────────────────────────────────────────────────
    QComboBox* degree_    = nullptr;
    QComboBox* accidental_= nullptr;
    QSpinBox*  octave_    = nullptr;
    QComboBox* denom_     = nullptr;
    QCheckBox* dotted_    = nullptr;
    QComboBox* tuplet_    = nullptr;
    QCheckBox* tie_       = nullptr;
    QComboBox* velocity_  = nullptr;
    QDoubleSpinBox* vel_num_ = nullptr;
    QComboBox* articulation_ = nullptr;
    QSpinBox*  repeat_    = nullptr;
    QSpinBox*  voice_     = nullptr;
    QCheckBox* chord_on_  = nullptr;
    QListWidget* chord_list_ = nullptr;
    QCheckBox* ov_atk_    = nullptr; QDoubleSpinBox* ov_atk_v_ = nullptr;
    QCheckBox* ov_dec_    = nullptr; QDoubleSpinBox* ov_dec_v_ = nullptr;
    QCheckBox* ov_sus_    = nullptr; QDoubleSpinBox* ov_sus_v_ = nullptr;
    QCheckBox* ov_br_     = nullptr; QDoubleSpinBox* ov_br_v_  = nullptr;
    QCheckBox* ov_B_      = nullptr; QDoubleSpinBox* ov_B_v_   = nullptr;
    QCheckBox* ov_pos_    = nullptr; QDoubleSpinBox* ov_pos_v_ = nullptr;
    QCheckBox* ov_damper_ = nullptr; QDoubleSpinBox* ov_damper_v_ = nullptr;
    QLineEdit* preview_   = nullptr;

    // ── 页 2 ────────────────────────────────────────────────────
    QListWidget*   preset_list_ = nullptr;
    QTextEdit*     preset_info_ = nullptr;
    QDoubleSpinBox* p_B_       = nullptr;
    QDoubleSpinBox* p_alpha_   = nullptr;
    QDoubleSpinBox* p_tilt_    = nullptr;
    QDoubleSpinBox* p_damp_    = nullptr;
    QDoubleSpinBox* p_n2_      = nullptr;
    QDoubleSpinBox* p_damper_  = nullptr;
    QDoubleSpinBox* p_ring_    = nullptr;
    QDoubleSpinBox* p_atk_     = nullptr;
    QDoubleSpinBox* p_sus_     = nullptr;
    QDoubleSpinBox* p_noise_   = nullptr;
    QDoubleSpinBox* p_pos_     = nullptr;
    QDoubleSpinBox* p_posp_    = nullptr;
    QSpinBox*       p_uni_     = nullptr;
    QDoubleSpinBox* p_detune_  = nullptr;
    QDoubleSpinBox* p_vtilt_   = nullptr;
    QDoubleSpinBox* p_velatk_  = nullptr;
    QDoubleSpinBox* p_beta_    = nullptr;
    QDoubleSpinBox* p_hpf_     = nullptr;
    QDoubleSpinBox* p_panp_    = nullptr;
    QDoubleSpinBox* p_stretch_ = nullptr;
    QSpinBox*       p_maxp_    = nullptr;
    QCheckBox*      p_random_phase_ = nullptr;
    QComboBox*      p_temper_  = nullptr;
    QLineEdit*      p_body_    = nullptr;   ///< f:q:db,f:q:db
    QLineEdit*      p_table_   = nullptr;   ///< ratio:amp:tau,...
    QLabel*         preset_head_ = nullptr;
    AcousticParams  current_    = Instruments::piano();
};

#endif // INSERTPAGE_H
