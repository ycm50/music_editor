#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTextEdit>
#include <QLineEdit>
#include <QComboBox>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>
#include <QProcess>
#include <QJsonObject>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * RCP 乐谱编辑器主窗口 (与手机端一致)
 *
 * 功能:
 *   - 单个编辑框持有完整 RCP 内容: 头部/BPM/基准频率/拍长/音色/持续比例/音符
 *   - 从编辑框内容直接解析渲染, 无需额外参数栏
 *   - AI 生成谱 (OpenAI 兼容接口, 流式输出)
 *   - 加载/保存 RCP 文件, 播放, 导出 WAV
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void on_load();
    void on_save();
    void on_play();
    void on_export_wav();
    void on_settings();
    void on_generate();
    void on_copy_prompt();
    void on_player_finished(int exit_code, QProcess::ExitStatus status);
    void on_save_finished(int exit_code, QProcess::ExitStatus status);

private:
    void setup_ui();
    QString editor_content() const;

    /// 将当前编辑框内容写入临时 RCP 文件, 返回路径
    QString write_tmp_rcp(const QString& suffix);

    // ── config.json 持久化 ──────────────────────────────────────
    QString config_path() const;
    void save_config();
    void load_config();
    /// 读取 config.json 根对象 (不存在或解析失败返回空对象)
    QJsonObject read_config() const;

    // ── AI ──────────────────────────────────────────────────────
    void start_generate(const QString& requirement, bool include_context);
    void stream_llm(const QString& system_prompt, const QString& user_prompt);
    QString call_once_llm(const QString& system_prompt, const QString& user_prompt);
    void handle_llm_delta(const QString& text);
    void handle_llm_done(const QString& full);
    QString api_base() const;
    QString api_base_raw() const;
    QString api_key() const;
    QString api_model() const;
    QStringList fetch_models();
    void toast(const QString& msg);
    void set_generating(bool on, const QString& status);
    /// 校验 RCP 格式, 返回错误消息 (空串表示合法)
    QString validate_rcp(const QString& content);

    // 控件
    QTextEdit*    note_editor_   = nullptr;
    QLineEdit*    prompt_input_  = nullptr;
    QCheckBox*    ctx_check_     = nullptr;
    QPushButton*  gen_btn_       = nullptr;
    QPushButton*  play_btn_      = nullptr;
    QPushButton*  export_btn_    = nullptr;

    // 进程
    QProcess*     player_proc_   = nullptr;
    QProcess*     save_proc_     = nullptr;

    // AI 生成累计文本
    QString       llm_accum_;

    // AI 网络
    QNetworkAccessManager* nam_  = nullptr;
    QNetworkReply* llm_reply_    = nullptr;

    // 状态
    bool          is_playing_    = false;
    QString       current_file_; // 当前文件路径 (为空表示新建)
    QString       play_tmp_path_;
};

#endif // MAINWINDOW_H
