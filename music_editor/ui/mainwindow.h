#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTextEdit>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QProcess>

/**
 * RCP 乐谱编辑器主窗口
 *
 * 功能:
 *   - 文本编辑乐谱 (省去头部参数行)
 *   - 设置 BPM / 基准频率 / 标准拍长
 *   - 选择音色
 *   - 加载/保存 RCP 文件
 *   - 播放 (调用 player 可执行文件)
 *   - 导出 WAV (调用 save 可执行文件)
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void on_load();
    void on_save();
    void on_play();
    void on_export_wav();
    void on_bpm_changed();
    void on_beat_dur_changed();
    void on_player_finished(int exit_code, QProcess::ExitStatus status);

private:
    void setup_ui();
    void sync_bpm_and_beat_dur(double bpm);

    // 控件
    QTextEdit*   note_editor_   = nullptr;
    QLineEdit*   bpm_entry_     = nullptr;
    QLineEdit*   freq_entry_    = nullptr;
    QLineEdit*   beat_dur_entry_= nullptr;
    QComboBox*   timbre_combo_  = nullptr;
    QPushButton* play_btn_      = nullptr;
    QPushButton* export_btn_    = nullptr;

    // 进程
    QProcess*    player_proc_   = nullptr;
    QProcess*    save_proc_     = nullptr;

    // 状态
    bool         is_playing_    = false;
    QString      current_file_; // 当前文件路径 (为空表示新建)
    QString      play_tmp_path_; // 播放用临时 RCP 文件路径
};

#endif // MAINWINDOW_H
