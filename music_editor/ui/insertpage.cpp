#include "insertpage.h"
#include "note_builder.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QTabWidget>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QTextEdit>
#include <QApplication>
#include <QClipboard>
#include <QScrollArea>
#include <QSplitter>
#include <QRegularExpression>

#include <algorithm>

namespace {
QDoubleSpinBox* make_spin(double lo, double hi, double step, int dec, double val)
{
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(dec);
    s->setValue(val);
    s->setKeyboardTracking(false);
    return s;
}
} // namespace

InsertPage::InsertPage(QTextEdit* target, QWidget* parent)
    : QWidget(parent)
    , target_(target)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    auto* tabs = new QTabWidget(this);
    root->addWidget(tabs);

    build_note_tab();
    build_instrument_tab();

    auto* note_tab = new QWidget(this);
    note_tab->setLayout(note_layout_);
    tabs->addTab(note_tab, "音符插入");

    auto* inst_tab = new QWidget(this);
    inst_tab->setLayout(inst_layout_);
    tabs->addTab(inst_tab, "乐器预设");

    refresh_preview();
    update_preset_view();
}

void InsertPage::insert_text(const QString& text)
{
    if (!target_ || text.isEmpty()) return;
    QTextCursor c = target_->textCursor();
    c.insertText(text);
    target_->setTextCursor(c);
    target_->setFocus();
}

// ── 页 1 ──────────────────────────────────────────────────────────
void InsertPage::build_note_tab()
{
    auto* outer = new QVBoxLayout;
    outer->setSpacing(6);

    auto* pitch = new QGroupBox("音高");
    auto* pf = new QHBoxLayout(pitch);
    pf->addWidget(new QLabel("音级"));
    degree_ = new QComboBox;
    const char* degs[] = {"1 do","2 re","3 mi","4 fa","5 sol","6 la","7 si","0 休止"};
    for (int i = 0; i < 8; ++i) degree_->addItem(degs[i], i < 7 ? i + 1 : 0);
    pf->addWidget(degree_);
    pf->addWidget(new QLabel("变音"));
    accidental_ = new QComboBox;
    accidental_->addItem("无", 0); accidental_->addItem("# 升", 1); accidental_->addItem("b 降", -1);
    accidental_->addItem("## 重升", 2); accidental_->addItem("bb 重降", -2);
    pf->addWidget(accidental_);
    pf->addWidget(new QLabel("八度"));
    octave_ = new QSpinBox; octave_->setRange(-4, 4); octave_->setValue(0);
    octave_->setPrefix("=");
    octave_->setToolTip("0 = 中音, += 高八度, -= 低八度");
    pf->addWidget(octave_);
    pf->addStretch();
    outer->addWidget(pitch);

    auto* dur = new QGroupBox("时值");
    auto* df = new QHBoxLayout(dur);
    df->addWidget(new QLabel("拍长"));
    denom_ = new QComboBox;
    denom_->addItem("全音符 1 (4拍)", 1);  denom_->addItem("二分 2 (2拍)", 2);
    denom_->addItem("四分 4 (1拍)", 4);    denom_->addItem("八分 8 (半拍)", 8);
    denom_->addItem("十六分 16", 16);      denom_->addItem("三十二分 32", 32);
    denom_->setCurrentIndex(2);
    df->addWidget(denom_);
    dotted_ = new QCheckBox("附点");
    dotted_->setToolTip("时值 ×1.5, 如 4. = 1.5 拍");
    df->addWidget(dotted_);
    df->addWidget(new QLabel("连音"));
    tuplet_ = new QComboBox;
    tuplet_->addItem("无", 0); tuplet_->addItem("三连音 t", 3); tuplet_->addItem("五连音 q", 5);
    df->addWidget(tuplet_);
    tie_ = new QCheckBox("与前一音连起来");
    tie_->setToolTip("用 _ 连接: 时值相加, 只起音一次");
    df->addWidget(tie_);
    df->addStretch();
    outer->addWidget(dur);

    auto* dyn = new QGroupBox("力度与演奏法 (力度同时改音色: 轻弹更暗, 重弹更亮更硬)");
    auto* dl = new QHBoxLayout(dyn);
    dl->addWidget(new QLabel("力度"));
    velocity_ = new QComboBox;
    for (const auto& n : velocity_names())
        velocity_->addItem(QString::fromStdString(n));
    velocity_->addItem("数值");
    velocity_->setCurrentIndex(6);
    dl->addWidget(velocity_);
    vel_num_ = make_spin(0.0, 4.0, 0.05, 2, 1.0);
    vel_num_->setEnabled(false);
    dl->addWidget(vel_num_);
    dl->addWidget(new QLabel("演奏法"));
    articulation_ = new QComboBox;
    articulation_->addItem("自动", 0);
    articulation_->addItem("连奏 ~", 1);
    articulation_->addItem("断奏 '", 2);
    dl->addWidget(articulation_);
    dl->addWidget(new QLabel("重复"));
    repeat_ = new QSpinBox; repeat_->setRange(1, 32); repeat_->setValue(1);
    dl->addWidget(repeat_);
    dl->addWidget(new QLabel("声部"));
    voice_ = new QSpinBox; voice_->setRange(0, 8); voice_->setValue(0);
    voice_->setToolTip(">0 时在 token 前插入 @voice N 行");
    dl->addWidget(voice_);
    dl->addStretch();
    outer->addWidget(dyn);

    auto* chord = new QGroupBox("和弦 (纵向叠加)");
    auto* cl = new QHBoxLayout(chord);
    chord_on_ = new QCheckBox("作为和弦发声");
    cl->addWidget(chord_on_);
    auto* add_btn = new QPushButton("把当前音加入和弦");
    auto* clr_btn = new QPushButton("清空");
    cl->addWidget(add_btn);
    cl->addWidget(clr_btn);
    cl->addStretch();
    outer->addWidget(chord);

    chord_list_ = new QListWidget;
    chord_list_->setMaximumHeight(60);
    outer->addWidget(chord_list_);

    auto* ov = new QGroupBox("逐音物理覆盖 ($ 后缀: 勾选后才写入)");
    auto* ovl = new QVBoxLayout(ov);
    auto add_ov = [&](const char* label, double lo, double hi, double step, int dec, double val,
                      QCheckBox** box, QDoubleSpinBox** sp) {
        auto* row = new QHBoxLayout;
        *box = new QCheckBox(label);
        *sp  = make_spin(lo, hi, step, dec, val);
        (*sp)->setEnabled(false);
        connect(*box, &QCheckBox::toggled, *sp, &QWidget::setEnabled);
        connect(*box, &QCheckBox::toggled, this, &InsertPage::refresh_preview);
        connect(*sp, &QDoubleSpinBox::valueChanged, this, &InsertPage::refresh_preview);
        row->addWidget(*box);
        row->addWidget(*sp);
        row->addStretch();
        ovl->addLayout(row);
    };
    add_ov("起音 atk (秒)", 0.0, 1.0, 0.002, 3, 0.010, &ov_atk_, &ov_atk_v_);
    add_ov("衰减 τ₁ (秒)", 0.01, 40.0, 0.05, 3, 1.000, &ov_dec_, &ov_dec_v_);
    add_ov("稳态 sus (0~1)", 0.0, 1.0, 0.05, 2, 0.50, &ov_sus_, &ov_sus_v_);
    add_ov("亮度 br (0~1)", 0.05, 1.0, 0.02, 2, 0.70, &ov_br_, &ov_br_v_);
    add_ov("非谐性 B", 0.0, 0.5, 0.0001, 5, 0.0002, &ov_B_, &ov_B_v_);
    add_ov("激励点 pos (x/L)", 0.0, 0.5, 0.01, 3, 0.125, &ov_pos_, &ov_pos_v_);
    add_ov("制音器 damper (秒)", 0.0, 2.0, 0.01, 3, 0.080, &ov_damper_, &ov_damper_v_);
    outer->addWidget(ov);

    auto* prev = new QGroupBox("预览与插入");
    auto* pv = new QVBoxLayout(prev);
    auto* prow = new QHBoxLayout;
    prow->addWidget(new QLabel("token"));
    preview_ = new QLineEdit;
    preview_->setReadOnly(true);
    preview_->setFont(QFont("Courier New", 11));
    prow->addWidget(preview_, 1);
    pv->addLayout(prow);
    auto* brow = new QHBoxLayout;
    auto* ins = new QPushButton("插入到光标处");
    auto* app = new QPushButton("追加到文末");
    auto* cp  = new QPushButton("复制");
    brow->addWidget(ins); brow->addWidget(app); brow->addWidget(cp); brow->addStretch();
    pv->addLayout(brow);
    outer->addWidget(prev);
    outer->addStretch();

    connect(degree_, &QComboBox::currentIndexChanged, this, &InsertPage::refresh_preview);
    connect(accidental_, &QComboBox::currentIndexChanged, this, &InsertPage::refresh_preview);
    connect(octave_, &QSpinBox::valueChanged, this, &InsertPage::refresh_preview);
    connect(denom_, &QComboBox::currentIndexChanged, this, &InsertPage::refresh_preview);
    connect(tuplet_, &QComboBox::currentIndexChanged, this, &InsertPage::refresh_preview);
    connect(dotted_, &QCheckBox::toggled, this, &InsertPage::refresh_preview);
    connect(tie_, &QCheckBox::toggled, this, &InsertPage::refresh_preview);
    connect(velocity_, &QComboBox::currentIndexChanged, this, [this](int) {
        vel_num_->setEnabled(velocity_->currentIndex() == velocity_->count() - 1);
        refresh_preview();
    });
    connect(vel_num_, &QDoubleSpinBox::valueChanged, this, &InsertPage::refresh_preview);
    connect(articulation_, &QComboBox::currentIndexChanged, this, &InsertPage::refresh_preview);
    connect(repeat_, &QSpinBox::valueChanged, this, &InsertPage::refresh_preview);
    connect(voice_, &QSpinBox::valueChanged, this, &InsertPage::refresh_preview);
    connect(chord_on_, &QCheckBox::toggled, this, &InsertPage::refresh_preview);
    connect(add_btn, &QPushButton::clicked, this, &InsertPage::on_add_chord_member);
    connect(clr_btn, &QPushButton::clicked, this, &InsertPage::on_clear_chord);
    connect(ins, &QPushButton::clicked, this, &InsertPage::on_insert_note);
    connect(app, &QPushButton::clicked, this, &InsertPage::on_append_note);
    connect(cp,  &QPushButton::clicked, this, &InsertPage::on_copy_note);

    note_layout_ = outer;
}

void InsertPage::on_add_chord_member()
{
    const NoteSpec s = current_spec();
    if (s.degree <= 0) return;
    const std::string core = render_note_core(s);
    if (core.empty()) return;
    chord_list_->addItem(QString::fromStdString(core));
    chord_on_->setChecked(true);
    refresh_preview();
}

void InsertPage::on_clear_chord()
{
    chord_list_->clear();
    refresh_preview();
}

NoteSpec InsertPage::current_spec() const
{
    NoteSpec s;
    s.degree      = degree_->currentData().toInt();
    s.accidental  = accidental_->currentData().toInt();
    s.octave      = octave_->value();
    s.denominator = denom_->currentData().toInt();
    s.dotted      = dotted_->isChecked();
    s.tuplet      = tuplet_->currentData().toInt();
    s.tie         = tie_->isChecked();
    s.articulation = articulation_->currentData().toInt();
    s.repeat      = repeat_->value();
    s.voice       = voice_->value();

    const int vc = velocity_->count();
    if (velocity_->currentIndex() >= vc - 1) {
        s.use_velocity = true;
        s.velocity_id = -1;
        s.velocity = vel_num_->value();
    } else {
        s.use_velocity = true;
        s.velocity_id = velocity_->currentIndex();
    }

    s.ov_atk = ov_atk_->isChecked();       s.atk = ov_atk_v_->value();
    s.ov_dec = ov_dec_->isChecked();       s.dec = ov_dec_v_->value();
    s.ov_sus = ov_sus_->isChecked();       s.sus = ov_sus_v_->value();
    s.ov_br  = ov_br_->isChecked();        s.br  = ov_br_v_->value();
    s.ov_B   = ov_B_->isChecked();         s.B   = ov_B_v_->value();
    s.ov_pos = ov_pos_->isChecked();       s.pos = ov_pos_v_->value();
    s.ov_damper = ov_damper_->isChecked(); s.damper = ov_damper_v_->value();

    if (chord_on_->isChecked()) {
        for (int i = 0; i < chord_list_->count(); ++i)
            s.chord_tokens.push_back(chord_list_->item(i)->text().toStdString());
    }
    return s;
}

void InsertPage::refresh_preview()
{
    const NoteSpec s = current_spec();
    QString token = QString::fromStdString(render_note_token(s));
    preview_->setText(token);
    preview_->setCursorPosition(0);
}

void InsertPage::on_insert_note()
{
    QString text = preview_->text();
    if (target_) {
        const QTextCursor cur = target_->textCursor();
        const QString before = target_->toPlainText().left(cur.position());
        const QString after  = target_->toPlainText().mid(cur.position());
        const bool need_pre  = !before.isEmpty() && !before.endsWith(' ')
                               && !before.endsWith('\n') && !before.endsWith('\t');
        const bool need_post = !after.isEmpty() && !after.startsWith(' ')
                               && !after.startsWith('\n') && !after.startsWith('\t');
        if (need_pre) text.prepend(' ');
        if (need_post) text.append(' ');
    }
    insert_text(text);
}

void InsertPage::on_append_note()
{
    if (!target_) return;
    QTextCursor c = target_->textCursor();
    c.movePosition(QTextCursor::End);
    target_->setTextCursor(c);
    QString text = preview_->text();
    text.prepend('\n');
    insert_text(text);
}

void InsertPage::on_copy_note()
{
    QApplication::clipboard()->setText(preview_->text());
}
// ── 页 2 ──────────────────────────────────────────────────────────
void InsertPage::build_instrument_tab()
{
    auto* outer = new QVBoxLayout;
    outer->setSpacing(6);

    auto* head = new QLabel("选中预设后可微调任意物理参数: 「应用 @timbre」写入简短乐器名; "
                            "「写入 @acoustic」把全部参数写成一行 (优先级最高, 可覆盖文件内的乐器)。");
    head->setWordWrap(true);
    outer->addWidget(head);

    preset_list_ = new QListWidget;
    preset_list_->setMinimumHeight(110);
    QString cat;
    for (const auto& info : instrument_catalog()) {
        const QString c = QString::fromStdString(info.category);
        if (c != cat) {
            cat = c;
            auto* h = new QListWidgetItem("── " + c + " ──");
            h->setFlags(Qt::NoItemFlags);
            preset_list_->addItem(h);
        }
        auto* it = new QListWidgetItem(QString::fromStdString(info.name).leftJustified(13)
                                       + QString::fromStdString(info.desc));
        it->setData(Qt::UserRole, QString::fromStdString(info.name));
        preset_list_->addItem(it);
    }
    outer->addWidget(preset_list_, 1);

    preset_info_ = new QTextEdit;
    preset_info_->setReadOnly(true);
    preset_info_->setFont(QFont("Courier New", 9));
    preset_info_->setMaximumHeight(110);
    outer->addWidget(preset_info_);

    auto* brow = new QHBoxLayout;
    auto* apply = new QPushButton("应用 @timbre");
    auto* write = new QPushButton("写入 @acoustic");
    auto* copy  = new QPushButton("复制 @acoustic");
    brow->addWidget(apply);
    brow->addWidget(write);
    brow->addWidget(copy);
    brow->addStretch();
    outer->addLayout(brow);

    auto* box = new QGroupBox("物理参数 (来自所选预设, 可随意修改)");
    auto* form = new QFormLayout(box);
    p_B_       = make_spin(0.0, 0.5, 1e-5, 6, 2e-4);      form->addRow("非谐性 B @C4", p_B_);
    p_alpha_   = make_spin(0.0, 3.0, 0.1, 2, 1.6);        form->addRow("B 随音高指数 alpha", p_alpha_);
    p_tilt_    = make_spin(0.0, 24.0, 0.5, 2, 6.0);       form->addRow("源频谱倾斜 (dB/oct)", p_tilt_);
    p_damp_    = make_spin(0.02, 40.0, 0.05, 3, 2.5);     form->addRow("t1 衰减 (秒)", p_damp_);
    p_n2_      = make_spin(0.0, 0.5, 0.002, 4, 0.020);    form->addRow("n2 高次分音阻尼", p_n2_);
    p_damper_  = make_spin(0.0, 2.0, 0.01, 3, 0.09);      form->addRow("制音器 t (秒, 0=自由余音)", p_damper_);
    p_ring_    = make_spin(0.1, 30.0, 0.5, 1, 8.0);       form->addRow("余音上限 (秒)", p_ring_);
    p_atk_     = make_spin(0.0005, 1.0, 0.002, 4, 0.003); form->addRow("起音 (秒)", p_atk_);
    p_sus_     = make_spin(0.0, 1.0, 0.05, 2, 0.0);       form->addRow("稳态 (0~1)", p_sus_);
    p_noise_   = make_spin(0.0, 1.0, 0.05, 2, 0.25);      form->addRow("起音噪声 (0~1)", p_noise_);
    p_pos_     = make_spin(0.0, 0.5, 0.01, 3, 0.125);     form->addRow("激励点 x/L (0=关)", p_pos_);
    p_posp_    = make_spin(0.0, 3.0, 0.1, 2, 1.6);        form->addRow("激励点指数 (2=1/n)", p_posp_);
    p_uni_     = new QSpinBox; p_uni_->setRange(1, 8);
    form->addRow("同音弦数", p_uni_);
    p_detune_  = make_spin(0.0, 50.0, 0.1, 2, 0.9);       form->addRow("弦间失谐 (音分)", p_detune_);
    p_vtilt_   = make_spin(0.0, 20.0, 0.5, 2, 5.0);       form->addRow("力度音色差 (dB/oct)", p_vtilt_);
    p_velatk_  = make_spin(0.0, 0.9, 0.05, 2, 0.45);      form->addRow("力度影响起音", p_velatk_);
    p_beta_    = make_spin(0.0, 0.9, 0.05, 2, 0.30);      form->addRow("双指数余韵 beta", p_beta_);
    p_hpf_     = make_spin(0.0, 500.0, 5.0, 1, 85.0);     form->addRow("低频滚降转折 (Hz)", p_hpf_);
    p_panp_    = make_spin(0.0, 1.0, 0.05, 2, 0.55);      form->addRow("声像随音高展开", p_panp_);
    p_stretch_ = make_spin(0.0, 1.0, 0.05, 2, 1.0);       form->addRow("伸展调音 (钢琴)", p_stretch_);
    p_maxp_    = new QSpinBox; p_maxp_->setRange(8, 1024);
    form->addRow("分音数上限", p_maxp_);
    p_random_phase_ = new QCheckBox("随机初相 (默认相干: 由激励点决定)");
    form->addRow(QString(), p_random_phase_);
    p_temper_ = new QComboBox;
    p_temper_->addItem("十二平均律", 0);
    p_temper_->addItem("纯律", 1);
    p_temper_->addItem("毕达哥拉斯律", 2);
    form->addRow("律制", p_temper_);
    p_body_ = new QLineEdit;
    p_body_->setPlaceholderText("共鸣体: 频率:Q:增益dB, 逗号分隔 (如 180:1.4:4,1400:1.2:3)");
    form->addRow("共鸣体", p_body_);
    p_table_ = new QLineEdit;
    p_table_->setPlaceholderText("显式分音表: 比率:振幅:t, 逗号分隔 (钟/马林巴; 留空=谐波列)");
    form->addRow("分音表", p_table_);

    auto* scroll = new QScrollArea;
    scroll->setWidget(box);
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll, 1);

    connect(preset_list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem*, QListWidgetItem*) { update_preset_view(); });
    connect(apply, &QPushButton::clicked, this, &InsertPage::on_apply_timbre);
    connect(write, &QPushButton::clicked, this, &InsertPage::on_insert_acoustic);
    connect(copy,  &QPushButton::clicked, this, &InsertPage::on_copy_acoustic);

    inst_layout_ = outer;
}

void InsertPage::load_preset_into_fields(const AcousticParams& p)
{
    p_B_->setValue(p.inharmonicity);
    p_alpha_->setValue(p.inharm_alpha);
    p_tilt_->setValue(p.tilt_db_oct);
    p_damp_->setValue(p.decay);
    p_n2_->setValue(p.damping_n2);
    p_damper_->setValue(p.damper);
    p_ring_->setValue(p.ring);
    p_atk_->setValue(p.attack);
    p_sus_->setValue(p.sustain);
    p_noise_->setValue(p.noise);
    p_pos_->setValue(p.strike_pos);
    p_posp_->setValue(p.strike_p);
    p_uni_->setValue(p.unison);
    p_detune_->setValue(p.detune_cents);
    p_vtilt_->setValue(p.vel_tilt);
    p_velatk_->setValue(p.vel_attack);
    p_beta_->setValue(p.decay2_beta);
    p_hpf_->setValue(p.body_hp_hz);
    p_panp_->setValue(p.pan_pitch);
    p_stretch_->setValue(p.stretch);
    p_maxp_->setValue(p.max_partials);
    p_random_phase_->setChecked(p.phase == PhaseMode::Random);
    p_temper_->setCurrentIndex(p.temperament == Temperament::Just ? 1
                              : (p.temperament == Temperament::Pythagorean ? 2 : 0));
    QString b;
    for (size_t i = 0; i < p.body.size(); ++i) {
        if (i) b += ",";
        b += QString("%1:%2:%3").arg(p.body[i].freq_hz).arg(p.body[i].q).arg(p.body[i].gain_db);
    }
    p_body_->setText(b);
    QString t;
    for (size_t i = 0; i < p.partial_table.size(); ++i) {
        if (i) t += ",";
        t += QString("%1:%2:%3").arg(p.partial_table[i].ratio)
                                .arg(p.partial_table[i].amp)
                                .arg(p.partial_table[i].tau);
    }
    p_table_->setText(t);
}

AcousticParams InsertPage::collect_params() const
{
    AcousticParams p = current_;
    p.inharmonicity = p_B_->value();
    p.inharm_alpha  = p_alpha_->value();
    p.tilt_db_oct   = p_tilt_->value();
    p.decay         = p_damp_->value();
    p.damping_law   = DampingLaw::TwoTerm;
    p.damping_n2    = p_n2_->value();
    p.partial_decay.clear();
    p.damper        = p_damper_->value();
    p.ring          = p_ring_->value();
    p.attack        = p_atk_->value();
    p.sustain       = p_sus_->value();
    p.noise         = p_noise_->value();
    p.strike_pos    = p_pos_->value();
    p.strike_p      = p_posp_->value();
    p.unison        = p_uni_->value();
    p.detune_cents  = p_detune_->value();
    p.vel_tilt      = p_vtilt_->value();
    p.vel_attack    = p_velatk_->value();
    p.decay2_beta   = p_beta_->value();
    p.body_hp_hz    = p_hpf_->value();
    p.body_hp_db_oct = p_hpf_->value() > 0.0 ? 6.0 : 0.0;
    p.pan_pitch     = p_panp_->value();
    p.stretch       = p_stretch_->value();
    p.max_partials  = p_maxp_->value();
    p.phase         = p_random_phase_->isChecked() ? PhaseMode::Random : PhaseMode::Coherent;
    p.temperament   = p_temper_->currentIndex() == 1 ? Temperament::Just
                      : (p_temper_->currentIndex() == 2 ? Temperament::Pythagorean
                                                        : Temperament::Equal12);
    p.body.clear();
    const QString bt = p_body_->text().trimmed();
    if (!bt.isEmpty()) {
        for (const QString& item : bt.split(',', Qt::SkipEmptyParts)) {
            const QStringList nums = item.trimmed().split(':', Qt::SkipEmptyParts);
            if (nums.size() >= 2) {
                BodyResonance br;
                br.freq_hz = nums[0].toDouble();
                if (nums.size() >= 3) { br.q = nums[1].toDouble(); br.gain_db = nums[2].toDouble(); }
                else                  { br.q = 6.0;              br.gain_db = nums[1].toDouble(); }
                p.body.push_back(br);
            }
        }
    }
    p.partial_table.clear();
    const QString tt = p_table_->text().trimmed();
    if (!tt.isEmpty()) {
        for (const QString& item : tt.split(',', Qt::SkipEmptyParts)) {
            const QStringList nums = item.trimmed().split(':', Qt::SkipEmptyParts);
            if (nums.size() >= 3) p.partial_table.push_back({nums[0].toDouble(), nums[1].toDouble(), nums[2].toDouble()});
        }
    }
    return p;
}

void InsertPage::update_preset_view()
{
    QListWidgetItem* item = preset_list_->currentItem();
    if (!item) return;
    const QString name = item->data(Qt::UserRole).toString();
    if (name.isEmpty()) return;
    const AcousticParams* preset = Instruments::find_by_name(name.toStdString());
    if (!preset) return;
    current_ = *preset;
    load_preset_into_fields(current_);

    const std::string raw_name = name.toStdString();
    QString info = QString::fromStdString(Instruments::category_of(raw_name)) + " · " + name + "\n";
    info += QString::fromStdString(render_acoustic_directive(current_)) + "\n";
    info += QString("分音数上限 %1, 制音器 %2, 同音弦组 %3, 激励点 %4\n")
                .arg(current_.max_partials)
                .arg(current_.damper > 0 ? QString::number(current_.damper) + " s" : QString("无(自由余音)"))
                .arg(current_.unison)
                .arg(current_.strike_pos);
    preset_info_->setPlainText(info);
}

void InsertPage::on_apply_timbre()
{
    QListWidgetItem* item = preset_list_->currentItem();
    if (!item) return;
    const QString name = item->data(Qt::UserRole).toString();
    if (name.isEmpty()) return;
    apply_directive_line("@timbre", render_timbre_directive(name.toStdString()).c_str());
}

void InsertPage::on_insert_acoustic()
{
    const AcousticParams p = collect_params();
    apply_directive_line("@acoustic", QString::fromStdString(render_acoustic_directive(p)));
}

void InsertPage::on_copy_acoustic()
{
    const AcousticParams p = collect_params();
    QApplication::clipboard()->setText(QString::fromStdString(render_acoustic_directive(p)));
}

void InsertPage::apply_directive_line(const QString& key, const QString& line)
{
    if (!target_) return;
    QStringList lines = target_->toPlainText().split('\n');
    int replaced = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].trimmed().startsWith(key)) { lines[i] = line; replaced = i; break; }
    }
    if (replaced < 0) {
        int pos = 0;
        static const QRegularExpression hdr("^\\s*\\d+(\\.\\d+)?\\s*,");
        for (int i = 0; i < lines.size(); ++i) {
            if (hdr.match(lines[i]).hasMatch()) { pos = i + 1; break; }
        }
        lines.insert(pos, line);
    }
    target_->setPlainText(lines.join('\n'));
}
