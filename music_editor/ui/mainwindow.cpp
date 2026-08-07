#include "mainwindow.h"
#include "tone_gen.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QDebug>
#include <QApplication>
#include <QClipboard>
#include <QStatusBar>
#include <QStandardPaths>
#include <QCloseEvent>
#include <QDir>
#include <QSettings>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTextCursor>
#include <QEventLoop>

#include <sstream>
#include <fstream>

// ── AI 生成提示词 (与手机端一致) ──────────────────────────────────
static const QString SCORE_SYSTEM_PROMPT =
    "你是简谱(数字谱)创作专家。根据用户要求生成 RCP 格式简谱，只输出 RCP 内容本身，不要任何解释、前后缀、代码块标记或歌词。\n"
    "RCP 由四部分组成，格式严格遵守：\n"
    "第 1 行头部：BPM,基准频率(Hz),标准拍长(秒)。这三个值由你根据用户要求自行选定并保证自洽：\n"
    "  - BPM 贴合情绪：缓慢抒情 60~90，从容中速 90~120，欢快激昂 120~180。\n"
    "  - 基准频率决定调性（1=do 的音高），如 C=261.63、D=293.66、F=349.23，一般取 200~440；用户指定调性时按用户要求。\n"
    "  - 标准拍长 = 60/BPM（一个四分音符的秒数），不要另外编一个对不上的数。\n"
    "  - 三者的组合要让总时长大致符合用户要求（如\"30秒左右\"）。\n"
    "第 2 行音色：N 个谐波振幅，用英文逗号分隔，第 1 个为基频(1倍频)振幅、第 2 个为 2 倍频振幅…依此类推，数值 0~1、可含小数，N 取 3~12，应贴合要求的情绪与风格。"
    "注意：振幅不必随倍频升高而递减，高倍频可以高于低倍频（如明亮、尖锐、鼻音等音色常强化高次谐波，第一个即基频通常可保持为 1）。\n"
    "第 3 行持续比例（必须写，与第 2 行一一对应，恰好 N 个，各项用英文感叹号 '!' 分隔，不要多也不要少）：每个倍频在单个音符时长内的存在区间，数值为音符时长的比例（整体为 1）。"
    "每个写 起点-终点：0-1 表示该倍频从始至终都有；0.3-0.5 表示只在音符的 0.3~0.5 处出现；0-0 表示该倍频所有时间不存在（静音）。"
    "一个倍频需要多个不连续区间时用花括号括起、内部用逗号分隔，如 {0.1-0.3,0.5-0.7}；区间末尾加 > 表示该区间内音量从第 2 行振幅逐渐降到 0，如 {0.1-0.3>,0.5-0.7} 表示 0.1~0.3 内渐弱到 0、0.5~0.7 稳定为第 2 行振幅。注意：> 只能紧跟某区间的终点数字，紧贴在 '}' 前面的区间加 > 时放在花括号内（如 {0.1-0.3>,0.5-0.7>}），严禁把 > 写在 '}' 之后或花括号外。"
    "用持续比例表现声音的起音/衰减/断续（如打击感强的高倍频短促出现，如 {0.05-0.3>,0.4-0.6}），不要都写 0-1。\n"
    "从第 4 行开始是音符，每行音符之间只用空格分隔，禁止 |、逗号、括号、连字符、歌词等任何其他符号。\n"
    "音符格式：音级.八度标记+拍长分母\n"
    "- 音级：1-7（do re mi fa sol la si），0 为休止。\n"
    "- 八度标记（必须写，不能省略）：0=中音，+=高八度，-=低八度，可连续（++=高两个八度，--=低两个八度）。\n"
    "- 拍长分母：4=四分音符(1拍)、2=二分音符(2拍)、8=八分音符(半拍)、1=全音符(4拍)、16=十六分音符(四分之一拍)。\n"
    "- 例：5.04=5音中音1拍；5.08=5音中音半拍；3.+4=mi高八度1拍；1.-4=do低八度1拍；0.04=休止1拍；0.08=休止半拍。\n"
    "- 长音换更小的分母数字（如 5.02=2拍、5.01=4拍），不要用 - 或任何延长记号。\n"
    "示例（欢快风格）：\n"
    "120,392,0.25\n"
    "1,0.7,0.5,0.3,0.2,0.1\n"
    "0-1!0-0.8!0-0.6!{0.1-0.3,0.5-0.7}!0-0.5!0-0.4\n"
    "1.+8 3.+8 5.+8 6.+8 5.+8 3.+8 1.+4 0.04\n"
    "2.+8 4.+8 6.+8 7.+8 6.+8 4.+8 2.+4 0.04\n"
    "5.04 5.08 6.08 7.08 1.+4 0.04\n"
    "创作要求：旋律、BPM、调性(基准频率)与音色都要贴合用户要求的情绪、风格与大致时长；"
    "欢快旋律多用 + 高八度音（1.+8、3.+8、5.+8 等）和 8/16 分音符，避免长音和低音。";

// ── 构造 / 析构 ──────────────────────────────────────────────────
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    nam_ = new QNetworkAccessManager(this);
    setup_ui();
    load_config();
}

MainWindow::~MainWindow()
{
    if (llm_reply_) llm_reply_->abort();
    if (player_proc_ && player_proc_->state() != QProcess::NotRunning) {
        player_proc_->kill();
        player_proc_->waitForFinished(1000);
    }
    if (save_proc_ && save_proc_->state() != QProcess::NotRunning) {
        save_proc_->kill();
        save_proc_->waitForFinished(1000);
    }
}

void MainWindow::closeEvent(QCloseEvent* /*event*/)
{
    // 关闭时自动持久化, 保证下一次启动恢复到上次内容
    save_config();
}

// ── 构建 UI ────────────────────────────────────────────────────────
void MainWindow::setup_ui()
{
    setWindowTitle("RCP 乐谱编辑器");
    resize(760, 640);

    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* main_layout = new QVBoxLayout(central);
    main_layout->setSpacing(8);

    // ── AI 生成区 ────────────────────────────────────────────────
    auto* ai_group = new QGroupBox("AI 生成谱", this);
    auto* ai_layout = new QVBoxLayout(ai_group);

    auto* prompt_row = new QHBoxLayout;
    prompt_input_ = new QLineEdit(this);
    prompt_input_->setPlaceholderText("生成要求，如：欢快的儿歌");
    prompt_row->addWidget(prompt_input_, 1);

    gen_btn_ = new QPushButton("生成谱", this);
    prompt_row->addWidget(gen_btn_);

    auto* ai_btn_row = new QHBoxLayout;
    ctx_check_ = new QCheckBox("把当前谱加入上下文", this);
    ai_btn_row->addWidget(ctx_check_);
    ai_btn_row->addStretch();
    auto* copy_prompt_btn = new QPushButton("复制提示词", this);
    ai_btn_row->addWidget(copy_prompt_btn);
    auto* settings_btn = new QPushButton("设置 (AI)", this);
    ai_btn_row->addWidget(settings_btn);

    ai_layout->addLayout(prompt_row);
    ai_layout->addLayout(ai_btn_row);
    main_layout->addWidget(ai_group);

    // ── 音符编辑器 (完整 RCP 内容, 与手机端一致) ─────────────────
    note_editor_ = new QTextEdit(this);
    note_editor_->setPlaceholderText(
        "在此输入完整 RCP 内容...\n"
        "第1行: BPM,基准频率(Hz),标准拍长(秒)\n"
        "第2行(可选): 音色, 各倍频振幅, 逗号分隔\n"
        "第3行(可选): 持续比例, 各倍频持续区间, 用 ! 分隔\n"
        "其后: 音符, 如 3.+2 表示 3 的高八度二分音符");
    note_editor_->setFont(QFont("Courier New", 11));
    note_editor_->setLineWrapMode(QTextEdit::NoWrap);
    main_layout->addWidget(note_editor_, 1);

    // ── 按钮区 ──────────────────────────────────────────────────
    auto* btn_layout = new QHBoxLayout;

    auto* load_btn = new QPushButton("加载 RCP", this);
    auto* save_btn = new QPushButton("保存 RCP", this);
    play_btn_     = new QPushButton("▶ 播放", this);
    export_btn_   = new QPushButton("导出 WAV", this);

    connect(load_btn, &QPushButton::clicked, this, &MainWindow::on_load);
    connect(save_btn, &QPushButton::clicked, this, &MainWindow::on_save);
    connect(play_btn_, &QPushButton::clicked, this, &MainWindow::on_play);
    connect(export_btn_, &QPushButton::clicked, this, &MainWindow::on_export_wav);
    connect(gen_btn_, &QPushButton::clicked, this, &MainWindow::on_generate);
    connect(settings_btn, &QPushButton::clicked, this, &MainWindow::on_settings);
    connect(copy_prompt_btn, &QPushButton::clicked, this, &MainWindow::on_copy_prompt);

    btn_layout->addWidget(load_btn);
    btn_layout->addWidget(save_btn);
    btn_layout->addStretch();
    btn_layout->addWidget(play_btn_);
    btn_layout->addWidget(export_btn_);

    main_layout->addLayout(btn_layout);

    statusBar()->showMessage("就绪");
}

QString MainWindow::editor_content() const
{
    return note_editor_->toPlainText();
}

// ── 临时文件 ───────────────────────────────────────────────────────
QString MainWindow::write_tmp_rcp(const QString& suffix)
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                   + suffix;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QTextStream out(&file);
    out << editor_content();
    return path;
}

// ── config.json 持久化 (编辑内容 + AI 设置) ────────────────────────
QString MainWindow::config_path() const
{
    return QDir(QApplication::applicationDirPath()).filePath("config.json");
}

QJsonObject MainWindow::read_config() const
{
    QFile file(config_path());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    // 去除可能的 UTF-8 BOM
    QByteArray raw = file.readAll();
    if (raw.startsWith("\xEF\xBB\xBF"))
        raw.remove(0, 3);
    auto doc = QJsonDocument::fromJson(raw);
    return doc.isObject() ? doc.object() : QJsonObject{};
}

void MainWindow::save_config()
{
    QJsonObject root = read_config();   // 保留已有 AI 设置

    root["content"] = editor_content();
    if (!current_file_.isEmpty())
        root["current_file"] = current_file_;

    QJsonObject ai = root.value("ai").toObject();
    ai["base_url"] = api_base_raw();
    ai["api_key"]  = api_key();
    ai["model"]    = api_model();
    root["ai"] = ai;

    QFile file(config_path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        statusBar()->showMessage("无法写入 config.json: " + file.errorString());
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    statusBar()->showMessage("已保存到 config.json");
}

void MainWindow::load_config()
{
    QJsonObject root = read_config();
    if (root.isEmpty())
        return;

    QString content = root.value("content").toString();
    if (!content.isEmpty())
        note_editor_->setPlainText(content);
    QString f = root.value("current_file").toString();
    if (!f.isEmpty())
        current_file_ = f;

    statusBar()->showMessage("已从 config.json 加载");
}

// ── 加载 / 保存 (编辑框即完整 RCP) ────────────────────────────────
void MainWindow::on_load()
{
    QString path = QFileDialog::getOpenFileName(this, "加载 RCP 文件", QString(), "RCP 文件 (*.rcp)");
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "错误", "无法打开文件:\n" + file.errorString());
        return;
    }
    QTextStream in(&file);
    note_editor_->setPlainText(in.readAll());
    current_file_ = path;
    statusBar()->showMessage("已加载: " + path);
}

void MainWindow::on_save()
{
    // 保存到 config.json, 下一次启动自动读取
    save_config();
}

// ── 播放 ───────────────────────────────────────────────────────────
void MainWindow::on_play()
{
    if (is_playing_) {
        if (player_proc_) {
            player_proc_->kill();
            player_proc_->waitForFinished(1000);
        }
        is_playing_ = false;
        play_btn_->setText("▶ 播放");
        statusBar()->showMessage("播放已停止");
        return;
    }

    if (editor_content().trimmed().isEmpty()) {
        toast("请输入乐谱内容");
        return;
    }

    play_tmp_path_ = write_tmp_rcp("/.tmp_play.rcp");
    if (play_tmp_path_.isEmpty()) {
        QMessageBox::warning(this, "错误", "无法创建临时文件");
        return;
    }

    QString player_path = QDir(QApplication::applicationDirPath())
                              .absoluteFilePath("player.exe");
    if (!QFile::exists(player_path)) {
        player_path = QDir(QApplication::applicationDirPath())
                          .absoluteFilePath("../player/player.exe");
    }
    if (!QFile::exists(player_path)) {
        QMessageBox::warning(this, "错误",
            "找不到 player 可执行文件:\n" + player_path + "\n"
            "请先编译 player 项目。");
        QFile::remove(play_tmp_path_);
        play_tmp_path_.clear();
        return;
    }

    if (player_proc_) {
        delete player_proc_;
        player_proc_ = nullptr;
    }
    player_proc_ = new QProcess(this);
    connect(player_proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::on_player_finished);

    QStringList args{play_tmp_path_};
    player_proc_->start(player_path, args);

    if (!player_proc_->waitForStarted(3000)) {
        QMessageBox::warning(this, "错误", "无法启动 player 进程:\n" + player_proc_->errorString());
        delete player_proc_;
        player_proc_ = nullptr;
        QFile::remove(play_tmp_path_);
        play_tmp_path_.clear();
        return;
    }

    is_playing_ = true;
    play_btn_->setText("⏹ 停止");
    statusBar()->showMessage("正在播放...");
}

void MainWindow::on_player_finished(int exit_code, QProcess::ExitStatus status)
{
    is_playing_ = false;
    play_btn_->setText("▶ 播放");

    if (status == QProcess::CrashExit) {
        statusBar()->showMessage("播放器异常退出");
    } else if (exit_code != 0) {
        QString err = player_proc_ ? player_proc_->readAllStandardError() : "";
        statusBar()->showMessage("播放出错 (code " + QString::number(exit_code) + "): " + err);
    } else {
        statusBar()->showMessage("播放完成");
    }

    if (!play_tmp_path_.isEmpty()) {
        QFile::remove(play_tmp_path_);
        play_tmp_path_.clear();
    }
}

// ── 导出 WAV ──────────────────────────────────────────────────────
void MainWindow::on_export_wav()
{
    QString output_path = QFileDialog::getSaveFileName(this,
        "导出 WAV 文件", "output.wav", "WAV 文件 (*.wav)");
    if (output_path.isEmpty()) return;

    QString tmp_path = write_tmp_rcp("/.tmp_export.rcp");
    if (tmp_path.isEmpty()) {
        QMessageBox::warning(this, "错误", "无法创建临时文件");
        return;
    }

    QString save_path = QDir(QApplication::applicationDirPath())
                            .absoluteFilePath("save.exe");
    if (!QFile::exists(save_path)) {
        save_path = QDir(QApplication::applicationDirPath())
                        .absoluteFilePath("../save/save.exe");
    }
    if (!QFile::exists(save_path)) {
        QMessageBox::warning(this, "错误",
            "找不到 save 可执行文件:\n" + save_path + "\n"
            "请先编译 save 项目。");
        QFile::remove(tmp_path);
        return;
    }

    if (save_proc_) {
        delete save_proc_;
        save_proc_ = nullptr;
    }
    save_proc_ = new QProcess(this);
    connect(save_proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::on_save_finished);

    QStringList args{tmp_path, "--output", output_path};
    save_proc_->start(save_path, args);

    if (!save_proc_->waitForStarted(3000)) {
        QMessageBox::warning(this, "错误", "无法启动 save 进程:\n" + save_proc_->errorString());
        delete save_proc_;
        save_proc_ = nullptr;
        QFile::remove(tmp_path);
        return;
    }
    statusBar()->showMessage("正在导出...");
}

void MainWindow::on_save_finished(int exit_code, QProcess::ExitStatus /*status*/)
{
    QString output_path = save_proc_ ? save_proc_->arguments().last() : QString();
    if (exit_code != 0) {
        QString err = save_proc_ ? save_proc_->readAllStandardError() : "";
        QMessageBox::warning(this, "错误",
            "WAV 导出失败 (code " + QString::number(exit_code) + "):\n" + err);
    } else {
        QString out = save_proc_ ? save_proc_->readAllStandardOutput() : "";
        statusBar()->showMessage("导出完成: " + output_path);

        auto reply = QMessageBox::question(this, "导出成功", out + "\n是否打开文件所在文件夹？",
                                            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
#ifdef Q_OS_WIN
            QProcess::startDetached("explorer", {"/select,", QDir::toNativeSeparators(output_path)});
#elif defined(Q_OS_MACOS)
            QProcess::startDetached("open", {"-R", output_path});
#else
            QFileInfo fi(output_path);
            QProcess::startDetached("xdg-open", {fi.absolutePath()});
#endif
        }
    }

    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/.tmp_export.rcp");
}

// ── AI 设置 ────────────────────────────────────────────────────────
void MainWindow::on_settings()
{
    QJsonObject cfg = read_config();
    QJsonObject ai  = cfg.value("ai").toObject();

    QDialog dlg(this);
    dlg.setWindowTitle("AI 设置 (OpenAI 兼容)");

    auto* form = new QFormLayout(&dlg);
    auto* base_edit = new QLineEdit(ai.value("base_url").toString(), &dlg);
    base_edit->setPlaceholderText("https://api.openai.com");
    auto* key_edit = new QLineEdit(ai.value("api_key").toString(), &dlg);
    key_edit->setEchoMode(QLineEdit::Password);
    auto* model_edit = new QLineEdit(ai.value("model").toString().isEmpty()
                                         ? "deepseek-chat"
                                         : ai.value("model").toString(), &dlg);
    auto* model_pick_btn = new QPushButton("获取模型列表", &dlg);

    auto* model_row = new QHBoxLayout;
    model_row->addWidget(model_edit, 1);
    model_row->addWidget(model_pick_btn);

    form->addRow("Base URL:", base_edit);
    form->addRow("API Key:", key_edit);
    form->addRow("模型:", model_row);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    form->addRow(btns);

    connect(model_pick_btn, &QPushButton::clicked, this, [this, model_edit, base_edit, key_edit] {
        QStringList models = fetch_models();
        if (models.isEmpty()) {
            toast("未获取到模型列表，请检查 Base URL / API Key");
            return;
        }
        bool ok = false;
        QString picked = QInputDialog::getItem(this, "选择模型", "模型:", models, 0, false, &ok);
        if (ok && !picked.isEmpty())
            model_edit->setText(picked);
    });
    connect(btns, &QDialogButtonBox::accepted, &dlg, [this, base_edit, key_edit, model_edit] {
        QJsonObject cfg = read_config();
        QJsonObject ai;
        ai["base_url"] = base_edit->text().trimmed();
        ai["api_key"]  = key_edit->text().trimmed();
        ai["model"]    = model_edit->text().trimmed();
        cfg["ai"] = ai;
        cfg["content"] = editor_content();
        if (!current_file_.isEmpty())
            cfg["current_file"] = current_file_;

        QFile file(config_path());
        if (file.open(QIODevice::WriteOnly | QIODevice::Text))
            file.write(QJsonDocument(cfg).toJson(QJsonDocument::Indented));
        toast("设置已保存");
    });
    connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    dlg.exec();
}

QString MainWindow::api_base_raw() const
{
    return read_config().value("ai").toObject().value("base_url").toString().trimmed();
}

QString MainWindow::api_base() const
{
    QString base = api_base_raw();
    if (base.isEmpty()) return {};
    if (!base.startsWith("http://") && !base.startsWith("https://"))
        base = "https://" + base;
    if (!base.endsWith("/v1")) base += "/v1";
    return base;
}

QString MainWindow::api_key() const
{
    return read_config().value("ai").toObject().value("api_key").toString().trimmed();
}

QString MainWindow::api_model() const
{
    QString m = read_config().value("ai").toObject().value("model").toString().trimmed();
    return m.isEmpty() ? "deepseek-chat" : m;
}

QStringList MainWindow::fetch_models()
{
    QString base = api_base();
    QString key = api_key();
    if (base.isEmpty() || key.isEmpty()) return {};

    QNetworkRequest req{QUrl(base + "/models")};
    req.setRawHeader("Authorization", ("Bearer " + key).toUtf8());

    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) return {};
    auto doc = QJsonDocument::fromJson(reply->readAll());
    if (!doc.isObject()) return {};
    QStringList out;
    for (const auto& v : doc.object().value("data").toArray())
        out << v.toObject().value("id").toString();
    return out;
}

// ── AI 生成 ────────────────────────────────────────────────────────
void MainWindow::on_generate()
{
    QString requirement = prompt_input_->text().trimmed();
    if (requirement.isEmpty()) {
        toast("请先填写生成要求");
        return;
    }
    if (api_base().isEmpty() || api_key().isEmpty()) {
        toast("请先在 设置 (AI) 中填写 Base URL 和 API Key");
        return;
    }
    start_generate(requirement, ctx_check_->isChecked());
}

void MainWindow::on_copy_prompt()
{
    QApplication::clipboard()->setText(SCORE_SYSTEM_PROMPT);
    toast("提示词已复制到剪贴板");
}

void MainWindow::start_generate(const QString& requirement, bool include_context)
{
    QString user_prompt = requirement;
    if (include_context) {
        QString ctx = editor_content().trimmed();
        if (!ctx.isEmpty())
            user_prompt = requirement + "\n\n以下是当前乐谱内容，可基于它续写、修改或优化，输出仍为完整 RCP（含头部、音色、持续比例、音符）：\n" + ctx;
    }

    // 清空编辑框, 从头流式输出
    note_editor_->clear();
    llm_accum_.clear();

    set_generating(true, "正在生成谱…");
    stream_llm(SCORE_SYSTEM_PROMPT, user_prompt);
}

void MainWindow::stream_llm(const QString& system_prompt, const QString& user_prompt)
{
    if (llm_reply_) {
        llm_reply_->abort();
        llm_reply_->deleteLater();
        llm_reply_ = nullptr;
    }

    QJsonObject sys{{"role", "system"}, {"content", system_prompt}};
    QJsonObject usr{{"role", "user"}, {"content", user_prompt}};
    QJsonArray messages;
    messages.append(sys);
    messages.append(usr);
    QJsonObject body{
        {"model", api_model()},
        {"temperature", 0.8},
        {"stream", true},
        {"messages", messages},
    };

    QNetworkRequest req{QUrl(api_base() + "/chat/completions")};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", ("Bearer " + api_key()).toUtf8());
    req.setRawHeader("Accept", "text/event-stream");

    llm_reply_ = nam_->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    llm_reply_->setProperty("user_prompt", user_prompt);
    llm_reply_->setProperty("system_prompt", system_prompt);

    QString* buf = new QString;
    connect(llm_reply_, &QNetworkReply::readyRead, this, [this, buf] {
        buf->append(QString::fromUtf8(llm_reply_->readAll()));
        // 按行解析 SSE
        int nl;
        while ((nl = buf->indexOf('\n')) >= 0) {
            QString line = buf->left(nl).trimmed();
            buf->remove(0, nl + 1);
            if (!line.startsWith("data:")) continue;
            QString data = line.mid(5).trimmed();
            if (data == "[DONE]") continue;
            auto doc = QJsonDocument::fromJson(data.toUtf8());
            if (!doc.isObject()) continue;
            QJsonValue content = doc.object().value("choices").toArray()
                                     .at(0).toObject().value("delta").toObject().value("content");
            if (content.isString()) {
                QString text = content.toString();
                if (text.isEmpty()) continue;
                handle_llm_delta(text);
            }
        }
    });
    connect(llm_reply_, &QNetworkReply::finished, this, [this, buf] {
        // 清残留缓冲
        if (!buf->isEmpty()) {
            QString line = buf->trimmed();
            if (line.startsWith("data:")) {
                QString data = line.mid(5).trimmed();
                if (data != "[DONE]") {
                    auto doc = QJsonDocument::fromJson(data.toUtf8());
                    if (doc.isObject()) {
                        QJsonValue content = doc.object().value("choices").toArray()
                                                 .at(0).toObject().value("delta").toObject().value("content");
                        if (content.isString() && !content.toString().isEmpty())
                            handle_llm_delta(content.toString());
                    }
                }
            }
        }
        delete buf;

        QNetworkReply* reply = llm_reply_;
        llm_reply_ = nullptr;
        QString user_prompt = reply->property("user_prompt").toString();
        QString system_prompt = reply->property("system_prompt").toString();

        if (reply->error() != QNetworkReply::NoError) {
            // 流式失败 → 回退一次性请求 (与手机端一致)
            QString once = call_once_llm(system_prompt, user_prompt);
            if (!once.isEmpty()) {
                llm_accum_ = once;
                note_editor_->setPlainText(once);
                handle_llm_done(once);
            } else {
                set_generating(false, "AI 调用失败: " + reply->errorString());
            }
            reply->deleteLater();
            return;
        }
        reply->deleteLater();
        // 全部 delta 已流式进入编辑框, 完成校验
        handle_llm_done(llm_accum_);
    });
}

QString MainWindow::call_once_llm(const QString& system_prompt, const QString& user_prompt)
{
    QJsonObject sys{{"role", "system"}, {"content", system_prompt}};
    QJsonObject usr{{"role", "user"}, {"content", user_prompt}};
    QJsonArray messages;
    messages.append(sys);
    messages.append(usr);
    QJsonObject body{
        {"model", api_model()},
        {"temperature", 0.8},
        {"stream", false},
        {"messages", messages},
    };

    QNetworkRequest req{QUrl(api_base() + "/chat/completions")};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", ("Bearer " + api_key()).toUtf8());

    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError)
        return {};
    auto doc = QJsonDocument::fromJson(reply->readAll());
    return doc.object().value("choices").toArray()
               .at(0).toObject().value("message").toObject().value("content").toString();
}

void MainWindow::handle_llm_delta(const QString& text)
{
    // 累加到独立缓冲, 避免与编辑框既有内容混淆
    llm_accum_ += text;
    note_editor_->setPlainText(llm_accum_);
    note_editor_->moveCursor(QTextCursor::End);
}

void MainWindow::handle_llm_done(const QString& full)
{
    if (full.trimmed().isEmpty()) {
        set_generating(false, "AI 返回为空");
        return;
    }

    QString err = validate_rcp(full);
    if (err.isEmpty())
        set_generating(false, "谱已生成，可直接播放");
    else
        set_generating(false, "谱已生成，但格式可能有误: " + err);
}

QString MainWindow::validate_rcp(const QString& content)
{
    try {
        // 以钢琴音色为回退, 文件内嵌音色优先
        auto audio = render_rcp_unified(content.toStdString(), Timbres::PIANO.harmonics, 44100);
        if (audio.empty())
            return "未生成音频";
        return {};
    } catch (const std::exception& e) {
        return QString::fromUtf8(e.what());
    }
}

void MainWindow::set_generating(bool on, const QString& status)
{
    gen_btn_->setEnabled(!on);
    statusBar()->showMessage(status);
}

void MainWindow::toast(const QString& msg)
{
    statusBar()->showMessage(msg, 5000);
}
