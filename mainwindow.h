#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSet>

QT_BEGIN_NAMESPACE
class QTreeWidget;
class QWebEngineView;
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void openChm();
    void onTreeItemActivated();

private:
    void createUi();
    bool unpackChm(const QString &chmPath, const QString &outDir);
    void buildFileTree(const QString &rootPath);
    void buildTocTree(const QString &hhcPath);
    void addFileToTree(const QString &filePath, const QString &rootPath);
    QByteArray detectEncoding(const QString &filePath);
    QString readFileWithEncoding(const QString &filePath, const QByteArray &encoding);
    void fixHtmlEncoding(const QString &htmlPath, const QByteArray &encoding);
    void cleanupTempDir();

    QTreeWidget *m_tree = nullptr;
    QWebEngineView *m_view = nullptr;
    QString m_tmpDir;
    QByteArray m_detectedEncoding;
    QSet<QString> m_convertedFiles;  // Track converted files to avoid re-conversion
};

#endif // MAINWINDOW_H
