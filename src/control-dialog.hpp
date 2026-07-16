#pragma once

#include <QDialog>

class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
struct obs_output;
typedef struct obs_output obs_output_t;

class BridgeControlDialog : public QDialog {
public:
  explicit BridgeControlDialog(QWidget *parent = nullptr);
  ~BridgeControlDialog() override;
  void stopOutput();

private:
  void startOutput();
  void refreshStatus();
  void createReceiverBundle();
  void applyRoleUi();
  void loadSettings();
  void saveSettings();
  QString uniqueSourceName(const QString &base) const;

  QComboBox *role_ = nullptr;
  QGroupBox *senderBox_ = nullptr;
  QGroupBox *receiverBox_ = nullptr;
  QLineEdit *ndiName_ = nullptr;
  QLineEdit *ndiGroups_ = nullptr;
  QComboBox *trackA_ = nullptr;
  QComboBox *trackB_ = nullptr;
  QPushButton *startStop_ = nullptr;
  QLabel *status_ = nullptr;
  QLabel *health_ = nullptr;
  QLineEdit *receiverNdiName_ = nullptr;
  QLineEdit *receiverBaseName_ = nullptr;
  QTimer *timer_ = nullptr;
  obs_output_t *output_ = nullptr;
};
