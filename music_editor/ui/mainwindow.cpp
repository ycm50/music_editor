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
#include <QStatusBar>
#include <QStandardPaths>
#include <QDir>

#include <sstream>
#include <fstream>

// ── 构造 / 析构 ──────────────────────────────────────────────────
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setup_ui();
}

MainWindow::~MainWindow()
{
    if (player_proc_ && player_proc_->state() != QProcess::NotRunning) {
        player_proc_->kill();
        player_proc_->waitForFinished(1000);
    }
    if (save_proc_ && save_proc_->state() != QProcess::NotRunning) {
        save_proc_->kill();
        save_proc_->waitForFinished(1000);
    }
}

// ── 构建 UI ────────────────────────────────────────────────────────
void MainWindow::setup_ui()
{
    setWindowTitle("RCP 乐谱编辑器");
    resize(700, 600);

    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* main_layout = new QVBoxLayout(central);
    main_layout->setSpacing(8);

    // ── 参数区 ──────────────────────────────────────────────────
    auto* param_group = new QGroupBox("乐谱参数", this);
    auto* param_layout = new QFormLayout(param_group);

    bpm_entry_ = new QLineEdit("388", this);
    freq_entry_ = new QLineEdit("261.63", this);
    beat_dur_entry_ = new QLineEdit("0.2", this);

    // BPM 变化时自动更新拍长
    connect(bpm_entry_, &QLineEdit::editingFinished, this, &MainWindow::on_bpm_changed);
    connect(beat_dur_entry_, &QLineEdit::editingFinished, this, &MainWindow::on_beat_dur_changed);

    param_layout->addRow("BPM:", bpm_entry_);
    param_layout->addRow("基准频率 (Hz):", freq_entry_);
    param_layout->addRow("标准拍长 (秒):", beat_dur_entry_);

    // 音色选择
    auto* timbre_layout = new QHBoxLayout;
    timbre_combo_ = new QComboBox(this);
    for (const auto& t : Timbres::all())
        timbre_combo_->addItem(QString::fromStdString(t.name));
    timbre_layout->addWidget(new QLabel("音色:", this));
    timbre_layout->addWidget(timbre_combo_, 1);
    param_layout->addRow("", timbre_layout);

    main_layout->addWidget(param_group);

    // ── 音符编辑器 ──────────────────────────────────────────────
    note_editor_ = new QTextEdit(this);
    note_editor_->setPlaceholderText(
        "在此输入音符...\n"
        "格式: <音>.<八度拍长>\n"
        "例: 3.+2 表示 3 的高八度二分音符\n"
        "音符用空格分隔");
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

    btn_layout->addWidget(load_btn);
    btn_layout->addWidget(save_btn);
    btn_layout->addStretch();
    btn_layout->addWidget(play_btn_);
    btn_layout->addWidget(export_btn_);

    main_layout->addLayout(btn_layout);

    // ── 状态栏 ──────────────────────────────────────────────────
    statusBar()->showMessage("就绪");
}

// ── BPM / 拍长同步 ────────────────────────────────────────────────
void MainWindow::on_bpm_changed()
{
    bool ok = false;
    double bpm = bpm_entry_->text().toDouble(&ok);
    if (ok && bpm > 0) {
        sync_bpm_and_beat_dur(bpm);
    }
}

void MainWindow::on_beat_dur_changed()
{
    bool ok = false;
    double dur = beat_dur_entry_->text().toDouble(&ok);
    if (ok && dur > 0) {
        double bpm = 60.0 / dur;
        bpm_entry_->blockSignals(true);
        bpm_entry_->setText(QString::number(bpm, 'f', 2));
        bpm_entry_->blockSignals(false);
    }
}

void MainWindow::sync_bpm_and_beat_dur(double bpm)
{
    double dur = 60.0 / bpm;
    beat_dur_entry_->blockSignals(true);
    beat_dur_entry_->setText(QString::number(dur, 'f', 6));
    beat_dur_entry_->blockSignals(false);
}

// ── 加载 RCP ───────────────────────────────────────────────────────
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
    QString header_line = in.readLine().trimmed();

    // 解析头部
    auto parts = header_line.split(',');
    if (parts.size() == 3) {
        bpm_entry_->setText(parts[0]);
        freq_entry_->setText(parts[1]);
        beat_dur_entry_->setText(parts[2]);
    }

    // 读取剩余内容
    QString rest = in.readAll();
    note_editor_->setPlainText(rest);

    current_file_ = path;
    statusBar()->showMessage("已加载: " + path);
}

// ── 保存 RCP ───────────────────────────────────────────────────────
void MainWindow::on_save()
{
    QString path = current_file_;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, "保存 RCP 文件", QString(), "RCP 文件 (*.rcp)");
        if (path.isEmpty()) return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "错误", "无法写入文件:\n" + file.errorString());
        return;
    }

    QTextStream out(&file);
    // 头部: BPM,base_freq,base_beat_duration
    out << bpm_entry_->text() << ","
        << freq_entry_->text() << ","
        << beat_dur_entry_->text() << "\n";
    // 音符内容
    out << note_editor_->toPlainText();

    current_file_ = path;
    statusBar()->showMessage("已保存: " + path);
}

// ── 播放 ───────────────────────────────────────────────────────────
void MainWindow::on_play()
{
    if (is_playing_) {
        // 停止播放
        if (player_proc_) {
            player_proc_->kill();
            player_proc_->waitForFinished(1000);
        }
        is_playing_ = false;
        play_btn_->setText("▶ 播放");
        statusBar()->showMessage("播放已停止");
        return;
    }

    // 先保存到临时文件
    play_tmp_path_ = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                       + "/.tmp_play.rcp";
    {
        QFile file(play_tmp_path_);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "错误", "无法创建临时文件");
            return;
        }
        QTextStream out(&file);
        out << bpm_entry_->text() << ","
            << freq_entry_->text() << ","
            << beat_dur_entry_->text() << "\n";
        out << note_editor_->toPlainText();
    }

    // 调用 player 可执行文件 (优先同目录平铺, 回退开发目录布局)
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
        return;
    }

    if (player_proc_) {
        delete player_proc_;
        player_proc_ = nullptr;
    }
    player_proc_ = new QProcess(this);
    connect(player_proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MainWindow::on_player_finished);

    QStringList args;
    args << play_tmp_path_
         << "--timbre" << timbre_combo_->currentText().toLower();

    player_proc_->start(player_path, args);

    if (!player_proc_->waitForStarted(3000)) {
        QMessageBox::warning(this, "错误", "无法启动 player 进程:\n" + player_proc_->errorString());
        delete player_proc_;
        player_proc_ = nullptr;
        if (!play_tmp_path_.isEmpty()) {
            QFile::remove(play_tmp_path_);
            play_tmp_path_.clear();
        }
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

    // 清理临时文件
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

    // 先保存到临时 RCP
    QString tmp_path = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                       + "/.tmp_export.rcp";
    {
        QFile file(tmp_path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "错误", "无法创建临时文件");
            return;
        }
        QTextStream out(&file);
        out << bpm_entry_->text() << ","
            << freq_entry_->text() << ","
            << beat_dur_entry_->text() << "\n";
        out << note_editor_->toPlainText();
    }

    // 调用 save 可执行文件 (优先同目录平铺, 回退开发目录布局)
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

    QStringList args;
    args << tmp_path
         << "--timbre" << timbre_combo_->currentText().toLower()
         << "--output" << output_path;

    save_proc_->start(save_path, args);

    if (!save_proc_->waitForFinished(300000)) {  // 最长 5 分钟
        QMessageBox::warning(this, "错误", "转换超时或失败");
        save_proc_->kill();
    } else if (save_proc_->exitCode() != 0) {
        QString err = save_proc_->readAllStandardError();
        QMessageBox::warning(this, "错误",
            "WAV 导出失败 (code " + QString::number(save_proc_->exitCode()) + "):\n" + err);
    } else {
        QString out = save_proc_->readAllStandardOutput();
        statusBar()->showMessage("导出完成: " + output_path);

        // 询问是否打开文件
        auto reply = QMessageBox::question(this, "导出成功", out + "\n是否打开文件所在文件夹？",
                                            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::Yes) {
            QFileInfo fi(output_path);
#ifdef Q_OS_WIN
            QProcess::startDetached("explorer", {"/select,", QDir::toNativeSeparators(output_path)});
#elif defined(Q_OS_MACOS)
            QProcess::startDetached("open", {"-R", output_path});
#else
            QProcess::startDetached("xdg-open", {fi.absolutePath()});
#endif
        }
    }

    QFile::remove(tmp_path);
}
