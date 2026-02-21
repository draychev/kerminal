#include <QApplication>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMainWindow>
#include <QPainter>
#include <QSocketNotifier>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct Cell {
  QChar ch = QChar(' ');
};

class ScreenBuffer {
 public:
  ScreenBuffer() { resize(1, 1); }
  void resize(int cols, int rows) {
    cols_ = std::max(1, cols);
    rows_ = std::max(1, rows);
    cells_.assign(cols_ * rows_, Cell());
    cursor_row_ = 0;
    cursor_col_ = 0;
  }

  int cols() const { return cols_; }
  int rows() const { return rows_; }

  void clear() {
    std::fill(cells_.begin(), cells_.end(), Cell());
    cursor_row_ = 0;
    cursor_col_ = 0;
  }

  void write(const QByteArray &data) {
    for (int i = 0; i < data.size(); ++i) {
      unsigned char byte = static_cast<unsigned char>(data[i]);
      if (esc_state_ == EscState::None) {
        if (byte == 0x1b) {
          esc_state_ = EscState::Esc;
          esc_params_.clear();
          esc_private_ = 0;
          continue;
        }
        handleByte(byte);
      } else {
        handleEscape(byte);
      }
    }
  }

  QString lineString(int row) const {
    if (row < 0 || row >= rows_) return QString();
    QString line;
    line.reserve(cols_);
    int offset = row * cols_;
    for (int col = 0; col < cols_; ++col) {
      line.append(cells_[offset + col].ch);
    }
    return line;
  }

 private:
  enum class EscState { None, Esc, Csi };

  void handleByte(unsigned char byte) {
    switch (byte) {
      case '\r':
        cursor_col_ = 0;
        return;
      case '\n':
        cursor_row_++;
        if (cursor_row_ >= rows_) {
          scrollUp();
          cursor_row_ = rows_ - 1;
        }
        return;
      case '\b':
        if (cursor_col_ > 0) cursor_col_--;
        return;
      case '\t': {
        int next_tab = ((cursor_col_ / 8) + 1) * 8;
        cursor_col_ = std::min(next_tab, cols_ - 1);
        return;
      }
      default:
        if (byte < 0x20) return;
        putChar(QChar::fromLatin1(static_cast<char>(byte)));
        return;
    }
  }

  void handleEscape(unsigned char byte) {
    if (esc_state_ == EscState::Esc) {
      if (byte == '[') {
        esc_state_ = EscState::Csi;
        esc_params_.clear();
        esc_private_ = 0;
      } else if (byte == 'c') {
        clear();
        esc_state_ = EscState::None;
      } else {
        esc_state_ = EscState::None;
      }
      return;
    }

    if (esc_state_ == EscState::Csi) {
      if (byte >= '0' && byte <= '9') {
        if (esc_params_.isEmpty()) esc_params_.append("0");
        esc_params_.back() = QString::number(esc_params_.back().toInt() * 10 + (byte - '0'));
        return;
      }
      if (byte == ';') {
        esc_params_.append("0");
        return;
      }
      if (byte == '?' || byte == '>') {
        esc_private_ = byte;
        return;
      }

      applyCsi(byte);
      esc_state_ = EscState::None;
      return;
    }
  }

  int param(int idx, int def = 1) const {
    if (idx < 0 || idx >= esc_params_.size()) return def;
    return esc_params_[idx].toInt();
  }

  void applyCsi(unsigned char command) {
    switch (command) {
      case 'A':
        cursor_row_ = std::max(0, cursor_row_ - param(0));
        break;
      case 'B':
        cursor_row_ = std::min(rows_ - 1, cursor_row_ + param(0));
        break;
      case 'C':
        cursor_col_ = std::min(cols_ - 1, cursor_col_ + param(0));
        break;
      case 'D':
        cursor_col_ = std::max(0, cursor_col_ - param(0));
        break;
      case 'G':
        cursor_col_ = std::clamp(param(0) - 1, 0, cols_ - 1);
        break;
      case 'H':
      case 'f':
        cursor_row_ = std::clamp(param(0) - 1, 0, rows_ - 1);
        cursor_col_ = std::clamp(param(1, 1) - 1, 0, cols_ - 1);
        break;
      case 'J':
        eraseInDisplay(param(0, 0));
        break;
      case 'K':
        eraseInLine(param(0, 0));
        break;
      case 'm':
        // Attribute changes ignored in this minimal renderer.
        break;
      case 's':
        saved_row_ = cursor_row_;
        saved_col_ = cursor_col_;
        break;
      case 'u':
        cursor_row_ = saved_row_;
        cursor_col_ = saved_col_;
        break;
      default:
        break;
    }
  }

  void eraseInLine(int mode) {
    int row = cursor_row_;
    if (row < 0 || row >= rows_) return;
    int start = 0;
    int end = cols_ - 1;
    if (mode == 0) {
      start = cursor_col_;
    } else if (mode == 1) {
      end = cursor_col_;
    }
    for (int col = start; col <= end; ++col) {
      cells_[row * cols_ + col] = Cell();
    }
  }

  void eraseInDisplay(int mode) {
    if (mode == 2) {
      clear();
      return;
    }
    if (mode == 0) {
      eraseInLine(0);
      for (int row = cursor_row_ + 1; row < rows_; ++row) {
        for (int col = 0; col < cols_; ++col) {
          cells_[row * cols_ + col] = Cell();
        }
      }
      return;
    }
    if (mode == 1) {
      for (int row = 0; row < cursor_row_; ++row) {
        for (int col = 0; col < cols_; ++col) {
          cells_[row * cols_ + col] = Cell();
        }
      }
      eraseInLine(1);
    }
  }

  void putChar(QChar ch) {
    if (cursor_row_ < 0 || cursor_row_ >= rows_) return;
    if (cursor_col_ < 0 || cursor_col_ >= cols_) return;
    cells_[cursor_row_ * cols_ + cursor_col_].ch = ch;
    cursor_col_++;
    if (cursor_col_ >= cols_) {
      cursor_col_ = 0;
      cursor_row_++;
      if (cursor_row_ >= rows_) {
        scrollUp();
        cursor_row_ = rows_ - 1;
      }
    }
  }

  void scrollUp() {
    if (rows_ <= 1) return;
    for (int row = 1; row < rows_; ++row) {
      std::copy_n(&cells_[row * cols_], cols_, &cells_[(row - 1) * cols_]);
    }
    for (int col = 0; col < cols_; ++col) {
      cells_[(rows_ - 1) * cols_ + col] = Cell();
    }
  }

  int cols_ = 1;
  int rows_ = 1;
  std::vector<Cell> cells_;
  int cursor_row_ = 0;
  int cursor_col_ = 0;
  int saved_row_ = 0;
  int saved_col_ = 0;
  EscState esc_state_ = EscState::None;
  QList<QString> esc_params_;
  char esc_private_ = 0;
};

class PaneWidget : public QWidget {
  Q_OBJECT
 public:
  explicit PaneWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
  }

  void setFontMetrics(const QFont &font) {
    font_ = font;
    QFontMetrics metrics(font_);
    cell_width_ = metrics.horizontalAdvance(QLatin1Char('M'));
    cell_height_ = metrics.height();
  }

  void setCellGeometry(int cols, int rows, int x, int y) {
    if (cols <= 0 || rows <= 0) return;
    if (buffer_.cols() != cols || buffer_.rows() != rows) {
      buffer_.resize(cols, rows);
    }
    setGeometry(x * cell_width_, y * cell_height_, cols * cell_width_, rows * cell_height_);
    update();
  }

  void setActive(bool active) {
    active_ = active;
    update();
  }

  void writeOutput(const QByteArray &data) {
    buffer_.write(data);
    update();
  }

  int cellWidth() const { return cell_width_; }
  int cellHeight() const { return cell_height_; }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(10, 10, 10));
    painter.setFont(font_);
    painter.setPen(QColor(220, 220, 220));

    int rows = buffer_.rows();
    for (int row = 0; row < rows; ++row) {
      painter.drawText(0, (row + 1) * cell_height_ - 2, buffer_.lineString(row));
    }

    if (active_) {
      QPen pen(QColor(80, 160, 255));
      pen.setWidth(2);
      painter.setPen(pen);
      painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
  }

  void keyPressEvent(QKeyEvent *event) override {
    emit keyPressed(event);
  }

 signals:
  void keyPressed(QKeyEvent *event);

 private:
  ScreenBuffer buffer_;
  QFont font_;
  int cell_width_ = 8;
  int cell_height_ = 16;
  bool active_ = false;
};

struct PaneInfo {
  QString id;
  QString window_id;
  int index = 0;
  bool active = false;
  PaneWidget *widget = nullptr;
};

struct WindowInfo {
  QString id;
  QString session_id;
  QString name;
  int index = 0;
  bool active = false;
  QString layout;
  QString active_pane_id;
};

struct SessionInfo {
  QString id;
  QString name;
  bool attached = false;
};

class TmuxClient : public QObject {
  Q_OBJECT
 public:
  explicit TmuxClient(QObject *parent = nullptr) : QObject(parent) {
  }

  void start() {
    if (pid_ > 0) return;
    pid_ = forkpty(&master_fd_, nullptr, nullptr, nullptr);
    if (pid_ == 0) {
      setenv("TERM", "xterm-256color", 1);
      execlp("tmux", "tmux", "-CC", "new", nullptr);
      _exit(1);
    }
    if (pid_ < 0) {
      emit tmuxExited();
      return;
    }

    int flags = fcntl(master_fd_, F_GETFL, 0);
    fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);

    notifier_ = new QSocketNotifier(master_fd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &TmuxClient::onReadyRead);

    wait_timer_ = new QTimer(this);
    wait_timer_->setInterval(500);
    connect(wait_timer_, &QTimer::timeout, this, &TmuxClient::checkChild);
    wait_timer_->start();
  }

  void sendCommand(const QString &command,
                   std::function<void(const QStringList &, bool)> callback = nullptr) {
    pending_.push_back({command, std::move(callback)});
    QByteArray bytes = command.toUtf8();
    bytes.append('\n');
    if (master_fd_ >= 0) {
      ::write(master_fd_, bytes.constData(), static_cast<size_t>(bytes.size()));
    }
  }

 signals:
  void notification(const QString &line);
  void outputData(const QString &pane_id, const QByteArray &data);
  void extendedOutputData(const QString &pane_id, const QByteArray &data);
  void commandOutputReady(const QString &command, const QStringList &lines, bool error);
  void tmuxExited();

 private:
  struct PendingCommand {
    QString command;
    std::function<void(const QStringList &, bool)> callback;
  };

  void onReadyRead() {
    if (master_fd_ < 0) return;
    QByteArray chunk;
    char buf[4096];
    while (true) {
      ssize_t n = ::read(master_fd_, buf, sizeof(buf));
      if (n > 0) {
        chunk.append(buf, static_cast<int>(n));
      } else {
        if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n == 0) {
          emit tmuxExited();
        }
        break;
      }
    }
    if (chunk.isEmpty()) return;
    buffer_.append(chunk);
    while (true) {
      int idx = buffer_.indexOf('\n');
      if (idx < 0) break;
      QByteArray line = buffer_.left(idx);
      buffer_ = buffer_.mid(idx + 1);
      if (!line.isEmpty() && line.endsWith('\r')) line.chop(1);
      parseLine(QString::fromUtf8(line));
    }
  }

  void checkChild() {
    if (pid_ <= 0) return;
    int status = 0;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
      emit tmuxExited();
      pid_ = -1;
    }
  }

  void parseLine(const QString &line) {
    if (in_output_) {
      if (line.startsWith("%end ")) {
        finishOutput(false);
        return;
      }
      if (line.startsWith("%error ")) {
        output_error_ = true;
        output_lines_.append(line);
        return;
      }
      output_lines_.append(line);
      return;
    }

    if (line.startsWith("%begin ")) {
      startOutput();
      return;
    }

    if (line.startsWith('%')) {
      if (line.startsWith("%output ")) {
        handleOutput(line, false);
      } else if (line.startsWith("%extended-output ")) {
        handleOutput(line, true);
      } else {
        emit notification(line);
      }
      return;
    }
  }

  void startOutput() {
    in_output_ = true;
    output_error_ = false;
    output_lines_.clear();
  }

  void finishOutput(bool forced) {
    in_output_ = false;
    if (pending_.empty()) return;
    PendingCommand cmd = pending_.front();
    pending_.pop_front();
    if (cmd.callback) cmd.callback(output_lines_, output_error_);
    emit commandOutputReady(cmd.command, output_lines_, output_error_);
    output_lines_.clear();
  }

  void handleOutput(const QString &line, bool extended) {
    QString rest = line.section(' ', 1);
    QString pane_id = rest.section(' ', 0, 0);
    QString data_part;
    if (extended) {
      data_part = rest.section(' ', 2);
    } else {
      data_part = rest.section(' ', 1);
    }
    QByteArray decoded = decodeTmuxEscapes(data_part);
    if (extended) emit extendedOutputData(pane_id, decoded);
    else emit outputData(pane_id, decoded);
  }

  QByteArray decodeTmuxEscapes(const QString &text) {
    QByteArray out;
    out.reserve(text.size());
    QByteArray bytes = text.toUtf8();
    for (int i = 0; i < bytes.size(); ++i) {
      char c = bytes[i];
      if (c != '\\') {
        out.append(c);
        continue;
      }
      if (i + 1 >= bytes.size()) break;
      char n = bytes[i + 1];
      if (n >= '0' && n <= '7') {
        int value = 0;
        int count = 0;
        while (i + 1 < bytes.size() && count < 3) {
          char o = bytes[i + 1];
          if (o < '0' || o > '7') break;
          value = (value * 8) + (o - '0');
          ++i;
          ++count;
        }
        out.append(static_cast<char>(value));
        continue;
      }
      switch (n) {
        case 'n': out.append('\n'); break;
        case 'r': out.append('\r'); break;
        case 't': out.append('\t'); break;
        case 'b': out.append('\b'); break;
        case 'f': out.append('\f'); break;
        case 'v': out.append('\v'); break;
        case 'e': out.append(0x1b); break;
        case '\\': out.append('\\'); break;
        default: out.append(n); break;
      }
      ++i;
    }
    return out;
  }

  int master_fd_ = -1;
  pid_t pid_ = -1;
  QSocketNotifier *notifier_ = nullptr;
  QTimer *wait_timer_ = nullptr;
  QByteArray buffer_;
  bool in_output_ = false;
  bool output_error_ = false;
  QStringList output_lines_;
  std::deque<PendingCommand> pending_;
};

class WindowSurface : public QWidget {
  Q_OBJECT
 public:
  explicit WindowSurface(QWidget *parent = nullptr) : QWidget(parent) {}

  void setFontMetrics(const QFont &font) {
    font_ = font;
    QFontMetrics metrics(font_);
    cell_width_ = metrics.horizontalAdvance(QLatin1Char('M'));
    cell_height_ = metrics.height();
  }

  int cellWidth() const { return cell_width_; }
  int cellHeight() const { return cell_height_; }

 private:
  QFont font_;
  int cell_width_ = 8;
  int cell_height_ = 16;
};

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow() {
    setWindowTitle("kerminal");
    statusBar();
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    tab_bar_ = new QTabBar(central);
    tab_bar_->setExpanding(false);
    tab_bar_->setDocumentMode(true);
    layout->addWidget(tab_bar_);

    surface_ = new WindowSurface(central);
    layout->addWidget(surface_, 1);

    setCentralWidget(central);

    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    surface_->setFontMetrics(font);

    connect(&tmux_, &TmuxClient::notification, this, &MainWindow::handleNotification);
    connect(&tmux_, &TmuxClient::outputData, this, &MainWindow::handlePaneOutput);
    connect(&tmux_, &TmuxClient::extendedOutputData, this, &MainWindow::handlePaneOutput);
    connect(&tmux_, &TmuxClient::tmuxExited, this, &MainWindow::close);
    connect(tab_bar_, &QTabBar::currentChanged, this, &MainWindow::handleWindowSelected);

    tmux_.start();

    QTimer::singleShot(100, this, &MainWindow::refreshAllState);
  }

 protected:
  void resizeEvent(QResizeEvent *event) override {
    QMainWindow::resizeEvent(event);
    updateClientSize();
  }

 private slots:
  void handleNotification(const QString &line) {
    if (line.startsWith("%exit")) {
      close();
      return;
    }
    if (line.startsWith("%pause")) {
      paused_ = true;
      statusBar()->showMessage("tmux output paused", 2000);
      return;
    }
    if (line.startsWith("%continue")) {
      paused_ = false;
      statusBar()->showMessage("tmux output resumed", 2000);
      return;
    }
    if (line.startsWith("%message ")) {
      statusBar()->showMessage(line.mid(9), 3000);
      return;
    }
    if (line.startsWith("%layout-change") ||
        line.startsWith("%window-add") ||
        line.startsWith("%window-close") ||
        line.startsWith("%window-renamed") ||
        line.startsWith("%window-unlinked") ||
        line.startsWith("%pane-add") ||
        line.startsWith("%pane-close") ||
        line.startsWith("%pane-mode-changed") ||
        line.startsWith("%pane-modified") ||
        line.startsWith("%paste-buffer-added") ||
        line.startsWith("%paste-buffer-changed") ||
        line.startsWith("%paste-buffer-deleted") ||
        line.startsWith("%session-changed") ||
        line.startsWith("%session-renamed") ||
        line.startsWith("%session-window-changed") ||
        line.startsWith("%sessions-changed") ||
        line.startsWith("%subscription-changed") ||
        line.startsWith("%unlinked-window-add") ||
        line.startsWith("%unlinked-window-close")) {
      refreshAllState();
    }
  }

  void handlePaneOutput(const QString &pane_id, const QByteArray &data) {
    auto it = panes_.find(pane_id);
    if (it == panes_.end() || it->second.widget == nullptr) {
      pending_output_[pane_id].append(data);
      return;
    }
    it->second.widget->writeOutput(data);
  }

  void handleWindowSelected(int index) {
    if (index < 0 || index >= window_order_.size()) return;
    QString window_id = window_order_[index];
    tmux_.sendCommand(QString("select-window -t %1").arg(window_id));
    active_window_id_ = window_id;
    layoutActiveWindow();
  }

 private:
  void updateClientSize() {
    if (!surface_) return;
    int cols = std::max(1, surface_->width() / surface_->cellWidth());
    int rows = std::max(1, surface_->height() / surface_->cellHeight());
    tmux_.sendCommand(QString("refresh-client -C %1,%2").arg(cols).arg(rows));
  }

  void refreshAllState() {
    updateClientSize();
    tmux_.sendCommand(
        "list-sessions -F \"#{session_id}\t#{session_name}\t#{session_attached}\"",
        [this](const QStringList &lines, bool) { parseSessions(lines); });
    tmux_.sendCommand(
        "list-windows -F \"#{session_id}\t#{window_id}\t#{window_index}\t#{window_name}\t#{window_active}\t#{window_layout}\t#{window_active_pane}\"",
        [this](const QStringList &lines, bool) { parseWindows(lines); });
    tmux_.sendCommand(
        "list-panes -s -F \"#{window_id}\t#{pane_id}\t#{pane_index}\t#{pane_active}\"",
        [this](const QStringList &lines, bool) { parsePanes(lines); });
  }

  void parseSessions(const QStringList &lines) {
    sessions_.clear();
    for (const QString &line : lines) {
      QStringList parts = line.split('\t');
      if (parts.size() < 3) continue;
      SessionInfo info;
      info.id = parts[0];
      info.name = parts[1];
      info.attached = (parts[2] == "1");
      sessions_[info.id] = info;
    }
  }

  void parseWindows(const QStringList &lines) {
    windows_.clear();
    window_order_.clear();
    for (const QString &line : lines) {
      QStringList parts = line.split('\t');
      if (parts.size() < 7) continue;
      WindowInfo info;
      info.session_id = parts[0];
      info.id = parts[1];
      info.index = parts[2].toInt();
      info.name = parts[3];
      info.active = (parts[4] == "1");
      info.layout = parts[5];
      info.active_pane_id = parts[6];
      windows_[info.id] = info;
      window_order_.push_back(info.id);
      if (info.active) active_window_id_ = info.id;
    }
    updateTabs();
    layoutActiveWindow();
  }

  void parsePanes(const QStringList &lines) {
    std::unordered_map<QString, PaneInfo> next;
    for (const QString &line : lines) {
      QStringList parts = line.split('\t');
      if (parts.size() < 4) continue;
      PaneInfo info;
      info.window_id = parts[0];
      info.id = parts[1];
      info.index = parts[2].toInt();
      info.active = (parts[3] == "1");
      auto it = panes_.find(info.id);
      if (it != panes_.end()) {
        info.widget = it->second.widget;
      } else {
        info.widget = new PaneWidget(surface_);
        info.widget->setFontMetrics(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        connect(info.widget, &PaneWidget::keyPressed, this, &MainWindow::forwardKeyPress);
      }
      auto pending_it = pending_output_.find(info.id);
      if (pending_it != pending_output_.end() && info.widget) {
        info.widget->writeOutput(pending_it->second);
        pending_output_.erase(pending_it);
      }
      next[info.id] = info;
    }

    for (auto &pair : panes_) {
      if (next.find(pair.first) == next.end()) {
        delete pair.second.widget;
      }
    }
    panes_.swap(next);
    layoutActiveWindow();
  }

  void updateTabs() {
    tab_bar_->blockSignals(true);
    while (tab_bar_->count() > 0) {
      tab_bar_->removeTab(0);
    }
    int active_index = 0;
    for (int i = 0; i < window_order_.size(); ++i) {
      const QString &window_id = window_order_[i];
      auto it = windows_.find(window_id);
      if (it == windows_.end()) continue;
      tab_bar_->addTab(QString("%1: %2").arg(it.value().index).arg(it.value().name));
      if (window_id == active_window_id_) active_index = i;
    }
    tab_bar_->setCurrentIndex(active_index);
    tab_bar_->blockSignals(false);
  }

  struct LayoutNode {
    int x = 0;
    int y = 0;
    int w = 1;
    int h = 1;
    bool is_leaf = false;
    QString pane_id;
    QVector<LayoutNode> children;
  };

  std::optional<LayoutNode> parseLayout(const QString &layout) {
    QString input = layout;
    int comma = input.indexOf(',');
    if (comma < 0) return std::nullopt;
    QString rest = input.mid(comma + 1);
    int pos = 0;
    auto node = parseLayoutNode(rest, pos);
    return node;
  }

  LayoutNode parseLayoutNode(const QString &text, int &pos) {
    LayoutNode node;
    readDimension(text, pos, node.w, node.h, node.x, node.y);
    if (pos >= text.size()) return node;
    QChar c = text[pos];
    if (c == '{' || c == '[') {
      QChar close = (c == '{') ? '}' : ']';
      ++pos;
      while (pos < text.size()) {
        LayoutNode child = parseLayoutNode(text, pos);
        node.children.append(child);
        if (pos >= text.size()) break;
        if (text[pos] == ',') {
          ++pos;
          continue;
        }
        if (text[pos] == close) {
          ++pos;
          break;
        }
      }
      node.is_leaf = false;
    } else if (c == ',') {
      ++pos;
      QString pane = readIdentifier(text, pos);
      node.is_leaf = true;
      node.pane_id = pane;
    }
    return node;
  }

  void readDimension(const QString &text, int &pos, int &w, int &h, int &x, int &y) {
    w = readNumber(text, pos);
    if (pos < text.size() && text[pos] == 'x') ++pos;
    h = readNumber(text, pos);
    if (pos < text.size() && text[pos] == ',') ++pos;
    x = readNumber(text, pos);
    if (pos < text.size() && text[pos] == ',') ++pos;
    y = readNumber(text, pos);
  }

  int readNumber(const QString &text, int &pos) {
    int value = 0;
    while (pos < text.size() && text[pos].isDigit()) {
      value = value * 10 + text[pos].digitValue();
      ++pos;
    }
    return value;
  }

  QString readIdentifier(const QString &text, int &pos) {
    QString out;
    while (pos < text.size()) {
      QChar c = text[pos];
      if (c == ',' || c == ']' || c == '}') break;
      out.append(c);
      ++pos;
    }
    return out.trimmed();
  }

  void layoutActiveWindow() {
    if (active_window_id_.isEmpty()) return;
    auto win_it = windows_.find(active_window_id_);
    if (win_it == windows_.end()) return;
    auto layout_opt = parseLayout(win_it.value().layout);
    if (!layout_opt) return;

    QHash<int, QString> index_to_id;
    for (auto &pair : panes_) {
      index_to_id.insert(pair.second.index, pair.first);
    }

    applyLayout(layout_opt.value(), index_to_id, win_it.value().active_pane_id);
  }

  void applyLayout(const LayoutNode &node, const QHash<int, QString> &index_to_id,
                   const QString &active_pane_id) {
    if (node.is_leaf) {
      QString pane_id = node.pane_id;
      if (!pane_id.startsWith('%')) {
        pane_id = index_to_id.value(pane_id.toInt(), pane_id);
      }
      auto it = panes_.find(pane_id);
      if (it == panes_.end()) return;
      PaneInfo &pane = it->second;
      pane.widget->setCellGeometry(node.w, node.h, node.x, node.y);
      pane.widget->setActive(pane.id == active_pane_id);
      pane.widget->show();
      return;
    }
    for (const LayoutNode &child : node.children) {
      applyLayout(child, index_to_id, active_pane_id);
    }
  }

  void forwardKeyPress(QKeyEvent *event) {
    QString target = activePaneId();
    if (target.isEmpty()) return;

    Qt::KeyboardModifiers mods = event->modifiers();
    int key = event->key();
    QString text = event->text();

    if (!text.isEmpty() && !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
      tmux_.sendCommand(QString("send-keys -t %1 -l -- %2").arg(target, escapeCommand(text)));
      return;
    }

    QString key_name = mapKey(event);
    if (!key_name.isEmpty()) {
      tmux_.sendCommand(QString("send-keys -t %1 %2").arg(target, key_name));
    }
  }

  QString escapeCommand(const QString &text) {
    QString escaped = text;
    escaped.replace("\\", "\\\\");
    escaped.replace("\n", "\\n");
    escaped.replace("\r", "\\r");
    escaped.replace("\t", "\\t");
    return escaped;
  }

  QString mapKey(QKeyEvent *event) {
    int key = event->key();
    Qt::KeyboardModifiers mods = event->modifiers();

    auto withModifier = [&](const QString &base) -> QString {
      QString prefix;
      if (mods & Qt::ControlModifier) prefix += "C-";
      if (mods & Qt::AltModifier) prefix += "M-";
      if (mods & Qt::MetaModifier) prefix += "M-";
      return prefix + base;
    };

    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) {
      return withModifier(QString("F%1").arg(key - Qt::Key_F1 + 1));
    }

    switch (key) {
      case Qt::Key_Return: return withModifier("Enter");
      case Qt::Key_Enter: return withModifier("Enter");
      case Qt::Key_Backspace: return withModifier("BSpace");
      case Qt::Key_Tab: return withModifier("Tab");
      case Qt::Key_Escape: return withModifier("Escape");
      case Qt::Key_Left: return withModifier("Left");
      case Qt::Key_Right: return withModifier("Right");
      case Qt::Key_Up: return withModifier("Up");
      case Qt::Key_Down: return withModifier("Down");
      case Qt::Key_Home: return withModifier("Home");
      case Qt::Key_End: return withModifier("End");
      case Qt::Key_PageUp: return withModifier("PageUp");
      case Qt::Key_PageDown: return withModifier("PageDown");
      case Qt::Key_Insert: return withModifier("Insert");
      case Qt::Key_Delete: return withModifier("Delete");
      case Qt::Key_Space:
        if (mods & Qt::ControlModifier) return "C-Space";
        break;
      default:
        break;
    }

    if ((mods & Qt::ControlModifier) && event->text().size() == 1) {
      QChar ch = event->text()[0].toLower();
      if (ch.isLetter()) return QString("C-%1").arg(ch);
    }

    if ((mods & Qt::AltModifier) && event->text().size() == 1) {
      QChar ch = event->text()[0];
      return QString("M-%1").arg(ch);
    }

    return QString();
  }

  QString activePaneId() const {
    auto win_it = windows_.find(active_window_id_);
    if (win_it == windows_.end()) return QString();
    return win_it.value().active_pane_id;
  }

  TmuxClient tmux_;
  QTabBar *tab_bar_ = nullptr;
  WindowSurface *surface_ = nullptr;
  QHash<QString, SessionInfo> sessions_;
  QHash<QString, WindowInfo> windows_;
  std::unordered_map<QString, PaneInfo> panes_;
  std::unordered_map<QString, QByteArray> pending_output_;
  QVector<QString> window_order_;
  QString active_window_id_;
  bool paused_ = false;
};

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  MainWindow window;
  window.resize(960, 600);
  window.show();
  return app.exec();
}

#include "main.moc"
