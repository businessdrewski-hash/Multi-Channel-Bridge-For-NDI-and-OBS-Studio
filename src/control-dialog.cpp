// Copyright (C) 2026 NDI Multichannel Bridge contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "control-dialog.hpp"
#include "bridge-common.hpp"

#include <obs-frontend-api.h>
#include <obs.h>

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

BridgeControlDialog::BridgeControlDialog(QWidget *parent) : QDialog(parent)
{
  setWindowTitle(obs_module_text("Dialog.Title"));
  resize(520, 430);

  auto *mainLayout = new QVBoxLayout(this);
  auto *senderBox = new QGroupBox(obs_module_text("Dialog.Sender"), this);
  auto *senderLayout = new QFormLayout(senderBox);

  ndiName_ = new QLineEdit(QStringLiteral("OBS Multichannel A/V"), senderBox);
  ndiGroups_ = new QLineEdit(senderBox);
  trackA_ = new QComboBox(senderBox);
  trackB_ = new QComboBox(senderBox);
  for (int i = 0; i < MAX_AUDIO_MIXES; ++i) {
    const QString label = tr("OBS Track %1").arg(i + 1);
    trackA_->addItem(label, i);
    trackB_->addItem(label, i);
  }
  trackA_->setCurrentIndex(0);
  trackB_->setCurrentIndex(1);

  senderLayout->addRow(obs_module_text("Dialog.NDIName"), ndiName_);
  senderLayout->addRow(obs_module_text("Dialog.Groups"), ndiGroups_);
  senderLayout->addRow(obs_module_text("Dialog.Pair12Track"), trackA_);
  senderLayout->addRow(obs_module_text("Dialog.Pair34Track"), trackB_);

  auto *senderButtons = new QHBoxLayout;
  startStop_ = new QPushButton(obs_module_text("Dialog.Start"), senderBox);
  status_ = new QLabel(obs_module_text("Dialog.Stopped"), senderBox);
  senderButtons->addWidget(startStop_);
  senderButtons->addWidget(status_, 1);
  senderLayout->addRow(senderButtons);
  mainLayout->addWidget(senderBox);

  auto *receiverBox = new QGroupBox(obs_module_text("Dialog.Receiver"), this);
  auto *receiverLayout = new QFormLayout(receiverBox);
  receiverNdiName_ = new QLineEdit(receiverBox);
  receiverBaseName_ = new QLineEdit(QStringLiteral("Multichannel NDI"), receiverBox);
  auto *createBundle = new QPushButton(obs_module_text("Dialog.CreateBundle"), receiverBox);
  receiverLayout->addRow(obs_module_text("Dialog.NDIName"), receiverNdiName_);
  receiverLayout->addRow(obs_module_text("Dialog.BundleName"), receiverBaseName_);
  receiverLayout->addRow(createBundle);
  auto *note = new QLabel(obs_module_text("Dialog.BundleNote"), receiverBox);
  note->setWordWrap(true);
  receiverLayout->addRow(note);
  mainLayout->addWidget(receiverBox);

  auto *closeButton = new QPushButton(obs_module_text("Dialog.Close"), this);
  mainLayout->addWidget(closeButton, 0, Qt::AlignRight);

  connect(startStop_, &QPushButton::clicked, this, [this] {
    if (output_ && obs_output_active(output_))
      stopOutput();
    else
      startOutput();
  });
  connect(createBundle, &QPushButton::clicked, this, [this] { createReceiverBundle(); });
  connect(closeButton, &QPushButton::clicked, this, &QDialog::hide);

  timer_ = new QTimer(this);
  timer_->setInterval(500);
  connect(timer_, &QTimer::timeout, this, [this] { refreshStatus(); });
  timer_->start();
}

BridgeControlDialog::~BridgeControlDialog()
{
  stopOutput();
}

void BridgeControlDialog::startOutput()
{
  if (trackA_->currentData().toInt() == trackB_->currentData().toInt()) {
    QMessageBox::warning(this, windowTitle(), obs_module_text("Dialog.DifferentTracks"));
    return;
  }

  stopOutput();
  obs_data_t *settings = obs_data_create();
  obs_data_set_string(settings, "ndi_name", ndiName_->text().toUtf8().constData());
  obs_data_set_string(settings, "ndi_groups", ndiGroups_->text().toUtf8().constData());
  obs_data_set_int(settings, "track_a", trackA_->currentData().toInt());
  obs_data_set_int(settings, "track_b", trackB_->currentData().toInt());

  output_ = obs_output_create(kOutputId, "NDI Multichannel Bridge Output", settings, nullptr);
  obs_data_release(settings);
  if (!output_) {
    QMessageBox::critical(this, windowTitle(), obs_module_text("Dialog.CreateFailed"));
    return;
  }

  obs_output_set_media(output_, obs_get_video(), obs_get_audio());
  const size_t mixerMask = (static_cast<size_t>(1) << trackA_->currentData().toInt()) |
                           (static_cast<size_t>(1) << trackB_->currentData().toInt());
  obs_output_set_mixers(output_, mixerMask);

  if (!obs_output_start(output_)) {
    const char *error = obs_output_get_last_error(output_);
    QMessageBox::critical(this, windowTitle(),
                          QString::fromUtf8(error && *error ? error : "Output failed to start"));
    obs_output_release(output_);
    output_ = nullptr;
  }
  refreshStatus();
}

void BridgeControlDialog::stopOutput()
{
  if (!output_)
    return;
  if (obs_output_active(output_))
    obs_output_force_stop(output_);
  obs_output_release(output_);
  output_ = nullptr;
  refreshStatus();
}

void BridgeControlDialog::refreshStatus()
{
  const bool active = output_ && obs_output_active(output_);
  startStop_->setText(active ? obs_module_text("Dialog.Stop") : obs_module_text("Dialog.Start"));
  status_->setText(active ? obs_module_text("Dialog.Running") : obs_module_text("Dialog.Stopped"));
}

QString BridgeControlDialog::uniqueSourceName(const QString &base) const
{
  QString name = base;
  int suffix = 2;
  while (obs_source_t *existing = obs_get_source_by_name(name.toUtf8().constData())) {
    obs_source_release(existing);
    name = QStringLiteral("%1 %2").arg(base).arg(suffix++);
  }
  return name;
}

void BridgeControlDialog::createReceiverBundle()
{
  const QString ndiName = receiverNdiName_->text().trimmed();
  if (ndiName.isEmpty()) {
    QMessageBox::warning(this, windowTitle(), obs_module_text("Dialog.EnterReceiverName"));
    return;
  }

  obs_source_t *sceneSource = obs_frontend_get_current_scene();
  obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
  if (!scene) {
    if (sceneSource)
      obs_source_release(sceneSource);
    QMessageBox::critical(this, windowTitle(), obs_module_text("Dialog.NoScene"));
    return;
  }

  const QString base = receiverBaseName_->text().trimmed().isEmpty()
                           ? QStringLiteral("Multichannel NDI")
                           : receiverBaseName_->text().trimmed();

  auto createAndAdd = [&](const char *id, const QString &name, int pairStart) {
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "ndi_source_name", ndiName.toUtf8().constData());
    obs_data_set_int(settings, "pair_start", pairStart);
    obs_source_t *source = obs_source_create(id, uniqueSourceName(name).toUtf8().constData(), settings, nullptr);
    obs_data_release(settings);
    if (source) {
      obs_scene_add(scene, source);
      obs_source_release(source);
      return true;
    }
    return false;
  };

  const bool videoOk = createAndAdd(kVideoSourceId, base + QStringLiteral(" - Video"), 0);
  const bool pairAOk = createAndAdd(kAudioSourceId, base + QStringLiteral(" - Desktop"), 0);
  const bool pairBOk = createAndAdd(kAudioSourceId, base + QStringLiteral(" - Mic"), 2);
  obs_source_release(sceneSource);

  if (videoOk && pairAOk && pairBOk)
    QMessageBox::information(this, windowTitle(), obs_module_text("Dialog.BundleCreated"));
  else
    QMessageBox::warning(this, windowTitle(), obs_module_text("Dialog.BundlePartial"));
}
