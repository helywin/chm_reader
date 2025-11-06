#include "mainwindow.h"

#include <QMenuBar>
#include <QAction>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QSplitter>
#include <QTreeWidget>
#include <QHeaderView>
#include <QDirIterator>
#include <QProcess>
#include <QTemporaryDir>
#include <QMessageBox>
#include <QFileInfo>
#include <QUrl>
#include <QFile>
#include <QApplication>

#include <QWebEngineView>
#include <QDir>
#include <QTextStream>
#include <QRegularExpression>
#include <QMap>
#include <QStack>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    createUi();
}

MainWindow::~MainWindow()
{
}

void MainWindow::createUi()
{
    auto openAct = new QAction(tr("Open CHM..."), this);
    connect(openAct, &QAction::triggered, this, &MainWindow::openChm);

    menuBar()->addAction(openAct);

    auto splitter = new QSplitter(this);

    m_tree = new QTreeWidget(splitter);
    m_tree->setHeaderLabels({tr("Name"), tr("Path")});
    m_tree->header()->setSectionHidden(1, true);
    m_tree->setColumnCount(2);

    m_view = new QWebEngineView(splitter);

    connect(m_tree, &QTreeWidget::itemActivated, this, &MainWindow::onTreeItemActivated);

    setCentralWidget(splitter);
    setWindowTitle(tr("CHM Reader"));
    resize(1000, 700);
}

void MainWindow::openChm()
{
    QString chmPath = QFileDialog::getOpenFileName(this, tr("Open CHM"), QString(), tr("CHM Files (*.chm);;All Files (*)"));
    if (chmPath.isEmpty()) return;

    // create temporary directory
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        QMessageBox::critical(this, tr("Error"), tr("Failed to create temporary directory."));
        return;
    }

    QString outDir = tmp.path();

    // Keep tmp alive by copying path into member and creating a QTemporaryDir pointer
    // We'll use m_tmpDir to store path; QTemporaryDir will be destroyed at end of scope
    // To avoid deletion while we use files, create a persistent directory
    QString persistentOut = QDir::temp().filePath(QString::fromUtf8("chmreader_%1").arg(QCoreApplication::applicationPid()));
    QDir().mkpath(persistentOut);

    bool ok = unpackChm(chmPath, persistentOut);
    if (!ok) {
        QMessageBox::critical(this, tr("Error"), tr("Failed to unpack CHM. Ensure p7zip (7z) is installed."));
        return;
    }

    m_tmpDir = persistentOut;
    
    // Detect encoding from first HTML file
    QDirIterator htmlIt(persistentOut, QStringList() << "*.html" << "*.htm", 
                        QDir::Files, QDirIterator::Subdirectories);
    if (htmlIt.hasNext()) {
        QString firstHtml = htmlIt.next();
        m_detectedEncoding = detectEncoding(firstHtml);
    } else {
        m_detectedEncoding = "UTF-8";
    }

    // populate tree with hierarchical structure
    m_tree->clear();
    
    // Try to find and parse .hhc (Table of Contents) file
    QString hhcPath;
    QStringList hhcCandidates = {"*.hhc"};
    QDirIterator hhcIt(persistentOut, hhcCandidates, QDir::Files, QDirIterator::Subdirectories);
    if (hhcIt.hasNext()) {
        hhcPath = hhcIt.next();
        buildTocTree(hhcPath);
    }
    
    // If no TOC found or TOC is empty, build file tree
    if (m_tree->topLevelItemCount() == 0) {
        buildFileTree(persistentOut);
    }

    // try to open index.html or default.htm
    QString candidates[] = {"index.html", "index.htm", "default.html", "default.htm"};
    for (const QString &c : candidates) {
        QString path = QDir(persistentOut).filePath(c);
        if (QFile::exists(path)) {
            // Fix encoding if needed (GBK to UTF-8)
            if (m_detectedEncoding != "UTF-8") {
                fixHtmlEncoding(path, m_detectedEncoding);
            }
            m_view->load(QUrl::fromLocalFile(path));
            break;
        }
    }
}

bool MainWindow::unpackChm(const QString &chmPath, const QString &outDir)
{
    // Use 7z x <chmPath> -o<outDir> -y
    QString program = "7z"; // p7zip provides 7z on Linux
    QStringList args;
    args << "x" << chmPath << QString("-o%1").arg(outDir) << "-y";

    QProcess proc;
    proc.start(program, args);
    bool started = proc.waitForStarted(5000);
    if (!started) return false;
    bool finished = proc.waitForFinished(30000);
    if (!finished) return false;

    int exitCode = proc.exitCode();
    return exitCode == 0;
}

void MainWindow::onTreeItemActivated()
{
    auto item = m_tree->currentItem();
    if (!item) return;
    QString path = item->text(1);
    if (path.isEmpty()) return;

    // Fix encoding for HTML files if needed
    if (m_detectedEncoding != "UTF-8" && 
        (path.endsWith(".html", Qt::CaseInsensitive) || path.endsWith(".htm", Qt::CaseInsensitive))) {
        fixHtmlEncoding(path, m_detectedEncoding);
    }
    
    // Only load HTML files; for others, try to open raw data or show as file://
    QUrl url = QUrl::fromLocalFile(path);
    m_view->load(url);
}

void MainWindow::buildFileTree(const QString &rootPath)
{
    QMap<QString, QTreeWidgetItem*> dirItems;
    dirItems[rootPath] = nullptr;
    
    QDirIterator it(rootPath, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    
    while (it.hasNext()) {
        it.next();
        QFileInfo fi = it.fileInfo();
        
        // Skip system files
        QString fileName = fi.fileName();
        if (fileName.startsWith('#') || fileName.startsWith('$')) {
            continue;
        }
        
        if (fi.isDir()) {
            // Create directory item
            QString parentPath = fi.absolutePath();
            QTreeWidgetItem *parentItem = dirItems.value(parentPath, nullptr);
            
            QTreeWidgetItem *dirItem;
            if (parentItem) {
                dirItem = new QTreeWidgetItem(parentItem);
            } else {
                dirItem = new QTreeWidgetItem(m_tree);
            }
            dirItem->setText(0, fileName);
            dirItem->setText(1, ""); // Directories don't have paths
            dirItems[fi.absoluteFilePath()] = dirItem;
        } else {
            // Create file item under its parent directory
            QString parentPath = fi.absolutePath();
            QTreeWidgetItem *parentItem = dirItems.value(parentPath, nullptr);
            
            QTreeWidgetItem *fileItem;
            if (parentItem) {
                fileItem = new QTreeWidgetItem(parentItem);
            } else {
                fileItem = new QTreeWidgetItem(m_tree);
            }
            fileItem->setText(0, fileName);
            fileItem->setText(1, fi.absoluteFilePath());
        }
    }
    
    m_tree->expandAll();
}

void MainWindow::buildTocTree(const QString &hhcPath)
{
    QFile file(hhcPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    
    // Detect encoding
    QByteArray encoding = detectEncoding(hhcPath);
    
    QTextStream in(&file);
    in.setCodec(encoding.constData());
    QString content = in.readAll();
    file.close();
    
    // Parse HTML-like .hhc file
    // .hhc files contain nested <UL> and <LI> with <OBJECT> containing <param> tags
    QStack<QTreeWidgetItem*> itemStack;
    itemStack.push(nullptr); // Root level
    
    QRegularExpression ulStart("<\\s*ul\\s*>", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression ulEnd("</\\s*ul\\s*>", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression liStart("<\\s*li\\s*>", QRegularExpression::CaseInsensitiveOption);
    QRegularExpression paramRx("<\\s*param\\s+name\\s*=\\s*\"([^\"]+)\"\\s+value\\s*=\\s*\"([^\"]+)\"", 
                               QRegularExpression::CaseInsensitiveOption);
    
    int pos = 0;
    QString currentName;
    QString currentLocal;
    
    while (pos < content.length()) {
        // Find next tag
        int ulStartPos = content.indexOf(ulStart, pos);
        int ulEndPos = content.indexOf(ulEnd, pos);
        int liStartPos = content.indexOf(liStart, pos);
        
        // Determine which comes first
        int nextPos = content.length();
        enum TagType { UL_START, UL_END, LI_START, NONE } nextTag = NONE;
        
        if (ulStartPos != -1 && ulStartPos < nextPos) { nextPos = ulStartPos; nextTag = UL_START; }
        if (ulEndPos != -1 && ulEndPos < nextPos) { nextPos = ulEndPos; nextTag = UL_END; }
        if (liStartPos != -1 && liStartPos < nextPos) { nextPos = liStartPos; nextTag = LI_START; }
        
        if (nextTag == NONE) break;
        
        if (nextTag == UL_START) {
            // Nothing to do, just move forward
            pos = nextPos + 4;
        } else if (nextTag == UL_END) {
            // Pop from stack
            if (itemStack.size() > 1) {
                itemStack.pop();
            }
            pos = nextPos + 5;
        } else if (nextTag == LI_START) {
            // Extract parameters until next </object> or next <li>
            int objEnd = content.indexOf("</object>", nextPos, Qt::CaseInsensitive);
            if (objEnd == -1) objEnd = content.indexOf("<li>", nextPos + 4, Qt::CaseInsensitive);
            if (objEnd == -1) objEnd = content.length();
            
            QString objContent = content.mid(nextPos, objEnd - nextPos);
            
            // Extract name and local parameters
            currentName.clear();
            currentLocal.clear();
            
            QRegularExpressionMatchIterator matchIt = paramRx.globalMatch(objContent);
            while (matchIt.hasNext()) {
                QRegularExpressionMatch match = matchIt.next();
                QString paramName = match.captured(1).toLower();
                QString paramValue = match.captured(2);
                
                if (paramName == "name") {
                    currentName = paramValue;
                } else if (paramName == "local") {
                    currentLocal = paramValue;
                }
            }
            
            // Create tree item if we have a name
            if (!currentName.isEmpty()) {
                QTreeWidgetItem *parentItem = itemStack.top();
                QTreeWidgetItem *newItem;
                
                if (parentItem) {
                    newItem = new QTreeWidgetItem(parentItem);
                } else {
                    newItem = new QTreeWidgetItem(m_tree);
                }
                
                newItem->setText(0, currentName);
                
                // Resolve local path relative to .hhc location
                if (!currentLocal.isEmpty()) {
                    QString absPath = QFileInfo(hhcPath).dir().filePath(currentLocal);
                    absPath = QFileInfo(absPath).absoluteFilePath();
                    newItem->setText(1, absPath);
                }
                
                // Check if next is <ul> (has children)
                int nextUlPos = content.indexOf(ulStart, objEnd);
                int nextLiPos = content.indexOf(liStart, objEnd);
                int nextUlEndPos = content.indexOf(ulEnd, objEnd);
                
                if (nextUlPos != -1 && (nextLiPos == -1 || nextUlPos < nextLiPos) && (nextUlEndPos == -1 || nextUlPos < nextUlEndPos)) {
                    // This item has children, push it to stack
                    itemStack.push(newItem);
                }
            }
            
            pos = objEnd;
        }
    }
    
    m_tree->expandToDepth(1);
}

QByteArray MainWindow::detectEncoding(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return "UTF-8";
    }
    
    // Read first 8KB to detect encoding
    QByteArray data = file.read(8192);
    file.close();
    
    QString dataStr = QString::fromLatin1(data);
    
    // Check for charset in meta tag
    QRegularExpression charsetRx("charset\\s*=\\s*['\"]?([^'\"\\s>]+)", 
                                  QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = charsetRx.match(dataStr);
    
    if (match.hasMatch()) {
        QString charset = match.captured(1).toUpper();
        
        // Map common Chinese charsets
        if (charset.contains("GBK") || charset.contains("GB2312") || 
            charset.contains("GB-2312") || charset.contains("CP936")) {
            return "GBK";
        } else if (charset.contains("BIG5")) {
            return "Big5";
        } else if (charset.contains("UTF-8") || charset.contains("UTF8")) {
            return "UTF-8";
        }
        
        return charset.toLatin1();
    }
    
    // Simple heuristic: check for GBK bytes
    // GBK first byte: 0x81-0xFE, second byte: 0x40-0xFE
    int gbkLikeCount = 0;
    int utf8LikeCount = 0;
    
    for (int i = 0; i < data.size() - 1; i++) {
        unsigned char c1 = static_cast<unsigned char>(data[i]);
        unsigned char c2 = static_cast<unsigned char>(data[i + 1]);
        
        // Check for GBK pattern
        if (c1 >= 0x81 && c1 <= 0xFE && c2 >= 0x40 && c2 <= 0xFE) {
            gbkLikeCount++;
        }
        
        // Check for UTF-8 pattern
        if ((c1 & 0xE0) == 0xE0 && (c2 & 0x80) == 0x80) {
            utf8LikeCount++;
        }
    }
    
    // If we find significant GBK patterns, use GBK
    if (gbkLikeCount > utf8LikeCount && gbkLikeCount > 5) {
        return "GBK";
    }
    
    // Default to UTF-8
    return "UTF-8";
}

QString MainWindow::readFileWithEncoding(const QString &filePath, const QByteArray &encoding)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    
    QTextStream in(&file);
    in.setCodec(encoding.constData());
    QString content = in.readAll();
    file.close();
    
    return content;
}

void MainWindow::fixHtmlEncoding(const QString &htmlPath, const QByteArray &encoding)
{
    // Read file with detected encoding
    QFile file(htmlPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    
    QTextStream in(&file);
    in.setCodec(encoding.constData());
    QString content = in.readAll();
    file.close();
    
    // Update or add charset meta tag
    QRegularExpression metaCharsetRx(
        "<meta\\s+[^>]*charset\\s*=\\s*['\"]?[^'\"\\s>]+['\"]?[^>]*>",
        QRegularExpression::CaseInsensitiveOption
    );
    
    QString newMetaTag = "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">";
    
    if (content.contains(metaCharsetRx)) {
        // Replace existing charset declaration
        content.replace(metaCharsetRx, newMetaTag);
    } else {
        // Add charset declaration after <head>
        QRegularExpression headRx("<head[^>]*>", QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch match = headRx.match(content);
        if (match.hasMatch()) {
            int insertPos = match.capturedEnd();
            content.insert(insertPos, "\n" + newMetaTag);
        }
    }
    
    // Write back as UTF-8
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << content;
    file.close();
}


