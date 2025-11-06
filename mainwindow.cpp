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
#include <QDebug>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QLabel>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    createUi();
}

MainWindow::~MainWindow()
{
    cleanupTempDir();
}

void MainWindow::createUi()
{
    auto openAct = new QAction(tr("Open CHM..."), this);
    connect(openAct, &QAction::triggered, this, &MainWindow::openChm);

    menuBar()->addAction(openAct);

    // Create splitter for tree and view
    auto splitter = new QSplitter(this);
    
    // Left panel: search bar + tree
    auto leftPanel = new QWidget(splitter);
    auto leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(5);
    
    // Create search bar
    auto searchLayout = new QHBoxLayout();
    searchLayout->setSpacing(3);
    m_searchEdit = new QLineEdit(leftPanel);
    m_searchEdit->setPlaceholderText(tr("Search..."));
    m_searchButton = new QPushButton(tr("Go"), leftPanel);
    m_searchButton->setMaximumWidth(40);
    m_clearSearchButton = new QPushButton(tr("Clear"), leftPanel);
    m_clearSearchButton->setMaximumWidth(50);
    
    searchLayout->addWidget(m_searchEdit);
    searchLayout->addWidget(m_searchButton);
    searchLayout->addWidget(m_clearSearchButton);
    
    connect(m_searchButton, &QPushButton::clicked, this, &MainWindow::onSearch);
    connect(m_clearSearchButton, &QPushButton::clicked, this, &MainWindow::onClearSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MainWindow::onSearch);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MainWindow::onSearchTextChanged);
    
    leftLayout->addLayout(searchLayout);

    // Tree widget
    m_tree = new QTreeWidget(leftPanel);
    m_tree->setHeaderLabels({tr("Name"), tr("Path")});
    m_tree->header()->setSectionHidden(1, true);
    m_tree->setColumnCount(2);
    
    leftLayout->addWidget(m_tree);

    // Right panel: web view
    m_view = new QWebEngineView(splitter);

    connect(m_tree, &QTreeWidget::itemActivated, this, &MainWindow::onTreeItemActivated);
    connect(m_view, &QWebEngineView::loadFinished, this, &MainWindow::onPageLoaded);
    
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);

    setCentralWidget(splitter);
    setWindowTitle(tr("CHM Reader"));
    resize(1000, 700);
}

void MainWindow::openChm()
{
    QString chmPath = QFileDialog::getOpenFileName(this, tr("Open CHM"), QString(), tr("CHM Files (*.chm);;All Files (*)"));
    if (chmPath.isEmpty()) return;

    // Clean up previous temporary directory if exists
    cleanupTempDir();

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
    
    // Clear converted files cache for new CHM
    m_convertedFiles.clear();
    
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
            // Detect and fix encoding if needed (only once)
            if (!m_convertedFiles.contains(path)) {
                QByteArray fileEncoding = detectEncoding(path);
                if (fileEncoding != "UTF-8") {
                    fixHtmlEncoding(path, fileEncoding);
                }
                m_convertedFiles.insert(path);
            }
            m_view->load(QUrl::fromLocalFile(path));
            break;
        }
    }
    
    // Clear search keyword when opening new CHM
    m_currentSearchKeyword.clear();
    m_searchEdit->clear();
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

    // Fix encoding for HTML files if needed (only once per file)
    if ((path.endsWith(".html", Qt::CaseInsensitive) || path.endsWith(".htm", Qt::CaseInsensitive))) {
        if (!m_convertedFiles.contains(path)) {
            // Detect encoding for this specific file
            QByteArray fileEncoding = detectEncoding(path);
            if (fileEncoding != "UTF-8") {
                fixHtmlEncoding(path, fileEncoding);
            }
            m_convertedFiles.insert(path);
        }
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
        
        qDebug() << "Found charset in meta tag:" << charset << "for file:" << filePath;
        
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
    qDebug() << "Converting file:" << htmlPath << "from encoding:" << encoding << "to UTF-8";
    
    // Read file with detected encoding
    QFile file(htmlPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open file for reading:" << htmlPath;
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

void MainWindow::cleanupTempDir()
{
    if (m_tmpDir.isEmpty()) {
        return;
    }
    
    QDir tmpDir(m_tmpDir);
    if (!tmpDir.exists()) {
        return;
    }
    
    qDebug() << "Cleaning up temporary directory:" << m_tmpDir;
    
    // Recursively remove the directory and all its contents
    if (tmpDir.removeRecursively()) {
        qDebug() << "Successfully removed temporary directory";
    } else {
        qDebug() << "Failed to remove temporary directory";
    }
}

void MainWindow::onSearch()
{
    QString keyword = m_searchEdit->text().trimmed();
    if (keyword.isEmpty()) {
        QMessageBox::information(this, tr("Search"), tr("Please enter a search keyword."));
        return;
    }
    
    if (m_tmpDir.isEmpty()) {
        QMessageBox::information(this, tr("Search"), tr("Please open a CHM file first."));
        return;
    }
    
    // Save search keyword for highlighting
    m_currentSearchKeyword = keyword;
    
    searchInFiles(keyword);
}

void MainWindow::onSearchTextChanged(const QString &text)
{
    // Enable/disable search button based on text
    m_searchButton->setEnabled(!text.trimmed().isEmpty());
}

void MainWindow::onClearSearch()
{
    // Clear search keyword
    m_currentSearchKeyword.clear();
    m_searchEdit->clear();
    
    if (m_tmpDir.isEmpty()) {
        return;
    }
    
    // Rebuild the original tree
    m_tree->clear();
    
    // Try to find and parse .hhc (Table of Contents) file
    QString hhcPath;
    QStringList hhcCandidates = {"*.hhc"};
    QDirIterator hhcIt(m_tmpDir, hhcCandidates, QDir::Files, QDirIterator::Subdirectories);
    if (hhcIt.hasNext()) {
        hhcPath = hhcIt.next();
        buildTocTree(hhcPath);
    }
    
    // If no TOC found or TOC is empty, build file tree
    if (m_tree->topLevelItemCount() == 0) {
        buildFileTree(m_tmpDir);
    }
}

void MainWindow::searchInFiles(const QString &keyword)
{
    m_tree->clear();
    
    QTreeWidgetItem *rootItem = new QTreeWidgetItem(m_tree);
    rootItem->setText(0, tr("Search Results: \"%1\"").arg(keyword));
    rootItem->setExpanded(true);
    
    int matchCount = 0;
    
    // Search in all HTML files
    QDirIterator it(m_tmpDir, QStringList() << "*.html" << "*.htm", 
                    QDir::Files, QDirIterator::Subdirectories);
    
    while (it.hasNext()) {
        QString filePath = it.next();
        QFileInfo fileInfo(filePath);
        
        // Skip system files
        if (fileInfo.fileName().startsWith('#') || fileInfo.fileName().startsWith('$')) {
            continue;
        }
        
        // Detect encoding for this file
        QByteArray encoding = detectEncoding(filePath);
        
        // Read file content
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        
        QTextStream in(&file);
        in.setCodec(encoding.constData());
        QString content = in.readAll();
        file.close();
        
        // Strip HTML tags for searching
        QString plainText = stripHtmlTags(content);
        
        // Search for keyword (case insensitive)
        if (plainText.contains(keyword, Qt::CaseInsensitive)) {
            matchCount++;
            
            // Extract title from HTML
            QString title = fileInfo.fileName();
            QRegularExpression titleRx("<title>([^<]+)</title>", QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch match = titleRx.match(content);
            if (match.hasMatch()) {
                title = match.captured(1).trimmed();
            }
            
            // Find context around the keyword
            int pos = plainText.indexOf(keyword, 0, Qt::CaseInsensitive);
            int contextStart = qMax(0, pos - 50);
            int contextEnd = qMin(plainText.length(), pos + keyword.length() + 50);
            QString context = plainText.mid(contextStart, contextEnd - contextStart).trimmed();
            if (contextStart > 0) context = "..." + context;
            if (contextEnd < plainText.length()) context = context + "...";
            
            auto item = new QTreeWidgetItem(rootItem);
            item->setText(0, QString("%1 - %2").arg(title, context));
            item->setText(1, filePath);
            item->setToolTip(0, context);
        }
    }
    
    rootItem->setText(0, tr("Search Results: \"%1\" (%2 matches)").arg(keyword).arg(matchCount));
    
    if (matchCount == 0) {
        auto item = new QTreeWidgetItem(rootItem);
        item->setText(0, tr("No results found"));
        item->setForeground(0, Qt::gray);
    }
}

QString MainWindow::stripHtmlTags(const QString &html)
{
    QString text = html;
    
    // Remove script and style tags with their content
    text.remove(QRegularExpression("<script[^>]*>.*</script>", QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
    text.remove(QRegularExpression("<style[^>]*>.*</style>", QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption));
    
    // Remove HTML tags
    text.remove(QRegularExpression("<[^>]*>"));
    
    // Decode HTML entities
    text.replace("&nbsp;", " ");
    text.replace("&lt;", "<");
    text.replace("&gt;", ">");
    text.replace("&amp;", "&");
    text.replace("&quot;", "\"");
    text.replace("&#39;", "'");
    
    // Normalize whitespace
    text = text.simplified();
    
    return text;
}

void MainWindow::onPageLoaded(bool ok)
{
    if (!ok) {
        return;
    }
    
    // Highlight search keyword if there is one
    if (!m_currentSearchKeyword.isEmpty()) {
        highlightKeyword(m_currentSearchKeyword);
    }
}

void MainWindow::highlightKeyword(const QString &keyword)
{
    if (keyword.isEmpty()) {
        return;
    }
    
    // JavaScript code to highlight text
    QString js = QString(R"(
        (function() {
            // Remove previous highlights
            var existingHighlights = document.querySelectorAll('.chm-search-highlight');
            existingHighlights.forEach(function(el) {
                var parent = el.parentNode;
                parent.replaceChild(document.createTextNode(el.textContent), el);
                parent.normalize();
            });
            
            // Function to highlight text in a node
            function highlightTextNode(node, keyword) {
                var text = node.nodeValue;
                var regex = new RegExp('(' + keyword.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + ')', 'gi');
                
                if (regex.test(text)) {
                    var span = document.createElement('span');
                    span.innerHTML = text.replace(regex, '<span class="chm-search-highlight" style="background-color: yellow; font-weight: bold;">$1</span>');
                    node.parentNode.replaceChild(span, node);
                    
                    // Scroll to first highlight
                    var firstHighlight = document.querySelector('.chm-search-highlight');
                    if (firstHighlight) {
                        firstHighlight.scrollIntoView({behavior: 'smooth', block: 'center'});
                    }
                }
            }
            
            // Walk through all text nodes
            function walkTextNodes(node) {
                if (node.nodeType === 3) { // Text node
                    highlightTextNode(node, '%1');
                } else if (node.nodeType === 1 && node.nodeName !== 'SCRIPT' && node.nodeName !== 'STYLE') {
                    for (var i = 0; i < node.childNodes.length; i++) {
                        walkTextNodes(node.childNodes[i]);
                    }
                }
            }
            
            walkTextNodes(document.body);
        })();
    )").arg(keyword);
    
    m_view->page()->runJavaScript(js);
}

