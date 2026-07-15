#pragma once

#include <QDialog>

class QComboBox;
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
  QString uniqueSourceName(const QString &base) const;

  QLineEdit *ndiName_ = nullptr;
  QLineEdit *ndiGroups_ = nullptr;
  QComboBox *trackA_ = nullptr;
  QComboBox *trackB_ = nullptr;
  QPushButton *startStop_ = nullptr;
  QLabel *status_ = nullptr;
  QLineEdit *receiverNdiName_ = nullptr;
  QLineEdit *receiverBaseName_ = nullptr;
  QTimer *timer_ = nullptr;
  obs_output_t *output_ = nullptr;
};
