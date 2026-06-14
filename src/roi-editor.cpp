#include "roi-editor.hpp"

#ifdef BUILD_STANDALONE
#include "external/display-helpers.hpp"
#include "external/qt-wrappers.hpp"
#else
#include "display-helpers.hpp"
#include "qt-wrappers.hpp"
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
#include <util/profiler.hpp>

#include <QAction>
#include <QFileDialog>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QObject>
#include <QMenu>

using namespace std;

RoiEditor *roi_edit;
static obs_hotkey_id roi_toggle_hotkey_id = OBS_INVALID_HOTKEY_ID;

/* A non-main canvas may carry a transition (e.g. an Aitum Vertical stinger)
 * on channel 0 instead of the scene itself. Resolve it to the actual active
 * scene so regions are matched against the right scene. Returns a new ref. */
static obs_source_t *GetCanvasActiveScene(obs_canvas_t *canvas)
{
	obs_source_t *channel = obs_canvas_get_channel(canvas, 0);
	if (!channel)
		return nullptr;

	if (obs_source_get_type(channel) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *active = obs_transition_get_active_source(channel);
		obs_source_release(channel);
		return active;
	}

	return channel;
}

/// ToDo cleanup this whole refresh mess, just rebuild data always when necessary,
/// and then update preview if visible, always run encoder update.

/* Translate the abstract priority percentage into the concrete effect it has
 * on NVENC, so users can tell that the useful range saturates well before
 * 100% (OBS maps priority to a QP offset: x51 for H.264/HEVC, x128 for AV1,
 * against a total QP range of 0-51 / 0-255). */
static QString PriorityHintText(int percent)
{
	if (percent == 0)
		return obs_module_text("ROI.PriorityHint.None");

	const float priority = (float)percent / 100.0f;
	const int qp_avc = (int)(-51.0f * priority);
	const int qp_av1 = (int)(-128.0f * priority);

	const int magnitude = abs(percent);
	const char *strength;
	if (magnitude <= 10)
		strength = "ROI.PriorityHint.Subtle";
	else if (magnitude <= 25)
		strength = "ROI.PriorityHint.Moderate";
	else if (magnitude <= 40)
		strength = "ROI.PriorityHint.Strong";
	else
		strength = "ROI.PriorityHint.Max";

	return QString(obs_module_text("ROI.PriorityHint"))
		.arg(qp_avc)
		.arg(qp_av1)
		.arg(obs_module_text(strength));
}

/* Qt only word-wraps tooltips when they are rich text, so plain tooltips
 * from the .ui files would render as one excessively long line. */
static void WordWrapToolTips(QWidget *root)
{
	const auto wrap = [](const QString &tip) {
		if (tip.isEmpty() || tip.startsWith("<"))
			return tip;
		return QString("<qt>%1</qt>").arg(tip.toHtmlEscaped());
	};

	root->setToolTip(wrap(root->toolTip()));
	for (QWidget *widget : root->findChildren<QWidget *>())
		widget->setToolTip(wrap(widget->toolTip()));
	for (QAction *action : root->findChildren<QAction *>())
		action->setToolTip(wrap(action->toolTip()));
}

RoiEditor::RoiEditor(QWidget *parent)
	: QDialog(parent),
	  ui(new Ui_ROIEditor),
	  geometry(QByteArray())
{
	ui->setupUi(this);

	/* Min/max buttons make this behave like a regular window, including
	 * Windows snap (Win+Arrow / drag to edge). */
	setWindowFlags((windowFlags() | Qt::WindowMinimizeButtonHint |
			Qt::WindowMaximizeButtonHint) &
		       ~Qt::WindowContextHelpButtonHint);

	// Work around Qt not allowing this as a custom property
	setThemeID(ui->roiWarningLabel, "warning");
	setThemeID(ui->roiErrorLabel, "error");

	// Hide properties until needed
	ui->roiPropertiesStack->setVisible(false);
	ui->roiCommonPropertiesGroupBox->setVisible(false);

	// Fill block size list
	ui->roiBlockSize->addItem(obs_module_text("ROI.BlockSize.16"), 16);
	ui->roiBlockSize->addItem(obs_module_text("ROI.BlockSize.32"), 32);
	ui->roiBlockSize->addItem(obs_module_text("ROI.BlockSize.64"), 64);
	ui->roiBlockSize->addItem(obs_module_text("ROI.BlockSize.128"), 128);

	// Fill smoothing options
	auto addSmoothingItem = [&](const char *text, int val) {
		ui->roiPropManualSmoothing->addItem(text, val);
		ui->roiPropSceneItemSmoothing->addItem(text, val);
	};

	addSmoothingItem(obs_module_text("ROI.Property.Smoothing.None"),
			 Smoothing::None);
	addSmoothingItem(obs_module_text("ROI.Property.Smoothing.Inside"),
			 Smoothing::Inside);
	addSmoothingItem(obs_module_text("ROI.Property.Smoothing.Outside"),
			 Smoothing::Outside);
	addSmoothingItem(obs_module_text("ROI.Property.Smoothing.Edge"),
			 Smoothing::Edge);

	connect(ui->close, &QPushButton::clicked, this, &RoiEditor::close);
	connect(ui->enableRoi, &QCheckBox::checkStateChanged, this,
		&RoiEditor::UpdateEncoders);
	connect(ui->excludeRecordings, &QCheckBox::checkStateChanged, this,
		&RoiEditor::UpdateEncoders);

	connect(ui->sceneSelect, &QComboBox::currentIndexChanged, this,
		&RoiEditor::SceneSelectionChanged);
	connect(ui->roiList, &QListWidget::currentItemChanged, this,
		&RoiEditor::ItemSelected);

	connect(ui->roiBlockSize, &QComboBox::currentIndexChanged, this,
		&RoiEditor::UpdatePreview);
	connect(ui->previewOpacity, &QSlider::valueChanged, this,
		&RoiEditor::UpdatePreview);

	connect(ui->roiPropPosX, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropPosY, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropSizeX, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropSizeY, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropPrioritySlider, &QSlider::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropSceneItem, &QComboBox::currentIndexChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropEnabled, &QCheckBox::checkStateChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropOuterPrioritySlider, &QSlider::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropStepsInnerSb, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropStepsOuterSb, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropRadiusInnerSb, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropRadiusOuterSb, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropRadiusOuterAspect, &QCheckBox::checkStateChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropRadiusInnerAspect, &QCheckBox::checkStateChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropManualSmoothingSteps, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropManualSmoothing, &QComboBox::currentIndexChanged,
		this, &RoiEditor::PropertiesChanges);
	connect(ui->roiPropManualSmoothingPriority, &QSlider::valueChanged,
		this, &RoiEditor::PropertiesChanges);
	connect(ui->roiPropCenterPosX, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropCenterPosY, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropRadiusInnerCircle, &QCheckBox::checkStateChanged, this,
		&RoiEditor::PropertiesChanges);
	connect(ui->roiPropSceneItemPadding, &QSpinBox::valueChanged, this,
		&RoiEditor::PropertiesChanges);

	connect(ui->roiPropPrioritySlider, &QSlider::valueChanged, this,
		[&](int value) {
			ui->roiPropPriorityHint->setText(
				PriorityHintText(value));
		});

	/* Per-codec priority overrides: slider/spinbox pairs, hidden unless
	 * customization is enabled for the selected region. */
	auto wirePerCodecRow = [&](QSlider *slider, QSpinBox *spinbox) {
		connect(slider, &QSlider::valueChanged, spinbox,
			&QSpinBox::setValue);
		connect(spinbox, &QSpinBox::valueChanged, slider,
			&QSlider::setValue);
		connect(slider, &QSlider::valueChanged, this,
			&RoiEditor::PropertiesChanges);
	};

	wirePerCodecRow(ui->roiPropPriorityH264, ui->roiPropPriorityH264Sb);
	wirePerCodecRow(ui->roiPropPriorityHEVC, ui->roiPropPriorityHEVCSb);
	wirePerCodecRow(ui->roiPropPriorityAV1, ui->roiPropPriorityAV1Sb);

	ui->roiPropPerCodecWidget->setVisible(false);

	connect(ui->roiPropPerCodec, &QCheckBox::checkStateChanged, this,
		[&](Qt::CheckState state) {
			const bool checked = state == Qt::Checked;
			ui->roiPropPerCodecWidget->setVisible(checked);

			/* Start the overrides from the base priority */
			if (checked &&
			    !ui->roiPropPriorityH264->value() &&
			    !ui->roiPropPriorityHEVC->value() &&
			    !ui->roiPropPriorityAV1->value()) {
				const int base =
					ui->roiPropPrioritySlider->value();
				QSignalBlocker b1(ui->roiPropPriorityH264);
				QSignalBlocker b2(ui->roiPropPriorityHEVC);
				QSignalBlocker b3(ui->roiPropPriorityAV1);
				ui->roiPropPriorityH264->setValue(base);
				ui->roiPropPriorityHEVC->setValue(base);
				ui->roiPropPriorityAV1->setValue(base);
				ui->roiPropPriorityH264Sb->setValue(base);
				ui->roiPropPriorityHEVCSb->setValue(base);
				ui->roiPropPriorityAV1Sb->setValue(base);
			}

			PropertiesChanges();
		});
	ui->roiPropPriorityHint->setText(
		PriorityHintText(ui->roiPropPrioritySlider->value()));

	ui->preview->installEventFilter(this);

	WordWrapToolTips(this);
}

void RoiEditor::CreateDisplay(bool recreate)
{
	// Need to recreate the display because it cannot be reset from the destroyed state
	if (recreate && ui->preview->GetDisplay() == nullptr) {
		int idx = ui->previewLayout->indexOf(ui->preview);
		QSizePolicy policy = ui->preview->sizePolicy();
		QSize minimum = ui->preview->minimumSize();

		delete ui->preview;
		ui->preview = new OBSQTDisplay(ui->layoutWidget_2);
		ui->preview->setObjectName("preview");
		ui->preview->setSizePolicy(policy);
		ui->preview->setMinimumSize(minimum);
		ui->preview->setToolTip(
			obs_module_text("ROI.Tooltip.Preview"));
		WordWrapToolTips(ui->preview);
		ui->preview->installEventFilter(this);

		ui->previewLayout->insertWidget(idx, ui->preview);
	}

	/* Regions are scaled from base (canvas) to output resolution when
	 * applied to encoders, so scaling no longer needs a warning. */
	ui->roiErrorLabel->setVisible(false);

	auto addDrawCallback = [this]() {
		obs_display_add_draw_callback(ui->preview->GetDisplay(),
					      RoiEditor::DrawPreview, this);
	};
	connect(ui->preview, &OBSQTDisplay::DisplayCreated, addDrawCallback);
}

bool RoiEditor::PreviewToCanvas(const QPointF &pos, uint32_t &canvas_x,
				uint32_t &canvas_y)
{
	const uint32_t base_width = editCanvasWidth;
	const uint32_t base_height = editCanvasHeight;
	if (!base_width || !base_height)
		return false;

	QSize size = GetPixelSize(ui->preview);
	int viewport_x, viewport_y;
	float scale;

	GetScaleAndCenterPos(base_width, base_height, size.width(),
			     size.height(), viewport_x, viewport_y, scale);

	if (scale <= 0.0f)
		return false;

	qreal dpr = ui->preview->devicePixelRatioF();
	double x = (pos.x() * dpr - viewport_x) / scale;
	double y = (pos.y() * dpr - viewport_y) / scale;

	canvas_x = (uint32_t)std::clamp(x, 0.0, (double)base_width);
	canvas_y = (uint32_t)std::clamp(y, 0.0, (double)base_height);
	return true;
}

bool RoiEditor::eventFilter(QObject *obj, QEvent *event)
{
	if (obj != ui->preview)
		return QDialog::eventFilter(obj, event);

	auto updateDragItem = [&](const QPointF &pos) {
		uint32_t x, y;
		if (!PreviewToCanvas(pos, x, y))
			return;

		RoiData roi = dragItem->data(ROIData).value<RoiData>();
		roi.posX = std::min(dragStartX, x);
		roi.posY = std::min(dragStartY, y);
		roi.width = std::max(
			dragStartX > x ? dragStartX - x : x - dragStartX,
			16u);
		roi.height = std::max(
			dragStartY > y ? dragStartY - y : y - dragStartY,
			16u);
		dragItem->setData(ROIData, QVariant::fromValue<RoiData>(roi));
	};

	switch (event->type()) {
	case QEvent::MouseButtonPress: {
		auto mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() != Qt::LeftButton)
			break;

		uint32_t x, y;
		if (!PreviewToCanvas(mouse->position(), x, y))
			break;

		dragStartX = x;
		dragStartY = y;

		dragItem = new RoiListItem(RoiListItem::Manual);
		RoiData roi = {};
		roi.enabled = true;
		roi.priority = 0.25f;
		roi.posX = x;
		roi.posY = y;
		roi.width = 16;
		roi.height = 16;
		dragItem->setData(ROIData,
				  QVariant::fromValue<RoiData>(roi));

		QSignalBlocker sb(ui->roiList);
		ui->roiList->insertItem(0, dragItem);
		return true;
	}
	case QEvent::MouseMove: {
		if (!dragItem)
			break;

		auto mouse = static_cast<QMouseEvent *>(event);
		updateDragItem(mouse->position());
		RegionItemsToData();
		UpdatePreview();
		return true;
	}
	case QEvent::MouseButtonRelease: {
		if (!dragItem)
			break;

		auto mouse = static_cast<QMouseEvent *>(event);
		updateDragItem(mouse->position());

		RoiListItem *item = dragItem;
		dragItem = nullptr;
		ui->roiList->setCurrentItem(item);
		RefreshData();
		return true;
	}
	default:
		break;
	}

	return QDialog::eventFilter(obj, event);
}

void RoiEditor::RefreshSceneList()
{
	QSignalBlocker sb(ui->sceneSelect);

	QVariant var = ui->sceneSelect->currentData();
	ui->sceneSelect->clear();

	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	for (size_t idx = 0; idx < scenes.sources.num; idx++) {
		obs_source_t *src = scenes.sources.array[idx];
		ui->sceneSelect->addItem(obs_source_get_name(src),
					 obs_source_get_uuid(src));
	}

	obs_frontend_source_list_free(&scenes);

	/* Scenes on additional canvases (e.g. a vertical canvas), labeled
	 * "Canvas: Scene". The main canvas is covered by the frontend list. */
	struct CanvasEnumCtx {
		QComboBox *cb;
		obs_canvas_t *main;
	};

	OBSCanvasAutoRelease main_canvas = obs_get_main_canvas();
	CanvasEnumCtx canvas_ctx{ui->sceneSelect, main_canvas.Get()};

	obs_enum_canvases(
		[](void *param, obs_canvas_t *canvas) -> bool {
			auto ctx = static_cast<CanvasEnumCtx *>(param);
			if (canvas == ctx->main)
				return true;

			struct SceneEnumCtx {
				QComboBox *cb;
				const char *canvas_name;
			} scene_ctx{ctx->cb, obs_canvas_get_name(canvas)};

			obs_canvas_enum_scenes(
				canvas,
				[](void *param, obs_source_t *src) -> bool {
					auto ctx = static_cast<SceneEnumCtx *>(
						param);
					ctx->cb->addItem(
						QString("%1: %2").arg(
							QT_UTF8(ctx->canvas_name),
							QT_UTF8(obs_source_get_name(
								src))),
						obs_source_get_uuid(src));
					return true;
				},
				&scene_ctx);
			return true;
		},
		&canvas_ctx);

	if (!var.isValid()) {
		OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
		if (scene)
			var = QString(obs_source_get_uuid(scene));
	}

	int idx = ui->sceneSelect->findData(var);
	ui->sceneSelect->setCurrentIndex(idx);
	UpdateEditCanvasSize();
}

/* Resolve the base resolution of the canvas that owns the scene currently
 * being edited, used by the preview and drag-to-draw coordinate mapping. */
void RoiEditor::UpdateEditCanvasSize()
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		editCanvasWidth = ovi.base_width;
		editCanvasHeight = ovi.base_height;
	}

	auto var = ui->sceneSelect->currentData();
	if (!var.isValid())
		return;

	const string scene_uuid = var.toString().toStdString();
	OBSSourceAutoRelease source =
		obs_get_source_by_uuid(scene_uuid.c_str());
	if (!source)
		return;

	OBSCanvasAutoRelease canvas = obs_source_get_canvas(source);
	if (!canvas)
		return;

	obs_video_info canvas_ovi;
	if (obs_canvas_get_video_info(canvas, &canvas_ovi)) {
		editCanvasWidth = canvas_ovi.base_width;
		editCanvasHeight = canvas_ovi.base_height;
	}
}

/* Combo box entries store a QVariantList of {item id, parent group item id}
 * so that sources inside groups can be targeted individually. The group id
 * is -1 for top-level items (including groups themselves). */
static QVariantList SceneItemComboData(int64_t item_id, int64_t group_id)
{
	return QVariantList{static_cast<qlonglong>(item_id),
			    static_cast<qlonglong>(group_id)};
}

struct SceneItemEnumCtx {
	QComboBox *cb;
	obs_sceneitem_t *group = nullptr;
};

static bool AddSceneItemToCombo(obs_scene_t *, obs_sceneitem_t *item,
				void *param)
{
	auto ctx = static_cast<SceneItemEnumCtx *>(param);

	QString name =
		QT_UTF8(obs_source_get_name(obs_sceneitem_get_source(item)));
	int64_t group_id = -1;

	if (ctx->group) {
		name = QString("%1 → %2").arg(
			QT_UTF8(obs_source_get_name(
				obs_sceneitem_get_source(ctx->group))),
			name);
		group_id = obs_sceneitem_get_id(ctx->group);
	}

	ctx->cb->addItem(name, SceneItemComboData(obs_sceneitem_get_id(item),
						  group_id));

	if (!ctx->group && obs_sceneitem_is_group(item)) {
		SceneItemEnumCtx child_ctx{ctx->cb, item};
		obs_sceneitem_group_enum_items(item, AddSceneItemToCombo,
					       &child_ctx);
	}

	return true;
}

void RoiEditor::RefreshSceneItems()
{
	if (!ui->roiPropSceneItem->isVisible())
		return;

	auto sceneVar = ui->sceneSelect->currentData();
	const string scene_uuid = sceneVar.toString().toStdString();
	OBSSourceAutoRelease source =
		obs_get_source_by_uuid(scene_uuid.c_str());

	if (!source)
		return;

	QSignalBlocker sb(ui->roiPropSceneItem);
	QVariant scene_item_data = ui->roiPropSceneItem->currentData();

	ui->roiPropSceneItem->clear();
	SceneItemEnumCtx ctx{ui->roiPropSceneItem};
	obs_scene_enum_items(obs_scene_from_source(source),
			     AddSceneItemToCombo, &ctx);

	if (scene_item_data.isValid()) {
		int idx = ui->roiPropSceneItem->findData(scene_item_data);
		if (idx != -1)
			ui->roiPropSceneItem->setCurrentIndex(idx);
	}
}

void RoiEditor::closeEvent(QCloseEvent *)
{
	geometry = saveGeometry();
	obs_frontend_save();

	// Clear some things we don't need while hidden
	QSignalBlocker sb(ui->roiList);
	ItemSelected(nullptr, nullptr);
	ui->roiList->clear();
}

/*
 * Slots
 */

void RoiEditor::SceneSelectionChanged()
{
	UpdateEditCanvasSize();
	RegionItemsFromData();
	UpdatePreview();
}

void RoiEditor::RefreshData()
{
	RegionItemsToData();
	UpdatePreview();
	UpdateEncoders();
}

void RoiEditor::PropertiesChanges()
{
	auto item = currentItem;
	if (!item)
		return;

	RoiData data = {};
	data.priority = (float)ui->roiPropPrioritySlider->value() / 100.0f;
	data.enabled = ui->roiPropEnabled->isChecked();
	data.per_codec_priority = ui->roiPropPerCodec->isChecked();
	data.priority_h264 =
		(float)ui->roiPropPriorityH264->value() / 100.0f;
	data.priority_hevc =
		(float)ui->roiPropPriorityHEVC->value() / 100.0f;
	data.priority_av1 = (float)ui->roiPropPriorityAV1->value() / 100.0f;
	// ToDo link those to the other ones
	data.smoothing_steps = ui->roiPropManualSmoothingSteps->value();
	data.smoothing_type = ui->roiPropManualSmoothing->currentData().toInt();
	data.smoothing_priority =
		(float)ui->roiPropManualSmoothingPriority->value() / 100.0f;

	if (item->type() == RoiListItem::SceneItem) {
		data.scene_item_name = ui->roiPropSceneItem->currentText();
		QVariantList ids = ui->roiPropSceneItem->currentData().toList();
		if (ids.size() == 2) {
			data.scene_item_id = ids[0].toLongLong();
			data.scene_item_group_id = ids[1].toLongLong();
		}
		data.padding = ui->roiPropSceneItemPadding->value();
	} else if (item->type() == RoiListItem::Manual) {
		data.posX = ui->roiPropPosX->value();
		data.posY = ui->roiPropPosY->value();
		data.width = ui->roiPropSizeX->value();
		data.height = ui->roiPropSizeY->value();
	} else if (item->type() == RoiListItem::CenterFocus) {
		data.inner_radius = ui->roiPropRadiusInnerSb->value();
		data.inner_aspect = ui->roiPropRadiusInnerAspect->isChecked();
		data.outer_radius = ui->roiPropRadiusOuterSb->value();
		data.outer_aspect = ui->roiPropRadiusOuterAspect->isChecked();
		data.inner_circle = ui->roiPropRadiusInnerCircle->isChecked();
		data.inner_steps = ui->roiPropStepsInnerSb->value();
		data.outer_steps = ui->roiPropStepsOuterSb->value();
		data.outer_priority =
			(float)ui->roiPropOuterPrioritySlider->value() / 100.0f;
		data.center_x = ui->roiPropCenterPosX->value();
		data.center_y = ui->roiPropCenterPosY->value();
	}

	item->setData(ROIData, QVariant::fromValue<RoiData>(data));

	RefreshData();
}

void RoiEditor::ItemSelected(QListWidgetItem *item, QListWidgetItem *)
{
	currentItem = nullptr;

	auto new_item = dynamic_cast<RoiListItem *>(item);
	if (!new_item) {
		ui->roiCommonPropertiesGroupBox->setVisible(false);
		ui->roiPropertiesStack->setVisible(false);
		return;
	}

	ui->roiCommonPropertiesGroupBox->setVisible(true);
	ui->roiPropertiesStack->setVisible(true);

	QVariant var = item->data(ROIData);
	RoiData data = var.value<RoiData>();

	// Generic properties
	ui->roiPropEnabled->setChecked(data.enabled);
	ui->roiPropPrioritySlider->setValue((int)(100 * data.priority));

	{
		QSignalBlocker pb(ui->roiPropPerCodec);
		ui->roiPropPerCodec->setChecked(data.per_codec_priority);
		ui->roiPropPerCodecWidget->setVisible(
			data.per_codec_priority);
	}
	ui->roiPropPriorityH264->setValue((int)(100 * data.priority_h264));
	ui->roiPropPriorityHEVC->setValue((int)(100 * data.priority_hevc));
	ui->roiPropPriorityAV1->setValue((int)(100 * data.priority_av1));

	// The "Manual" widgets act as a master for all other ones for the same values
	ui->roiPropManualSmoothingPriority->setValue(
		(int)(100 * data.smoothing_priority));
	ui->roiPropManualSmoothingSteps->setValue(data.smoothing_steps);

	int idx = ui->roiPropManualSmoothing->findData(data.smoothing_type);
	if (idx != -1)
		ui->roiPropManualSmoothing->setCurrentIndex(idx);

	// Type specific properties
	if (item->type() == RoiListItem::SceneItem) {
		ui->roiPropertiesStack->setCurrentWidget(
			ui->roiSceneItemPropertiesGroupBox);

		RefreshSceneItems();
		int idx = ui->roiPropSceneItem->findData(SceneItemComboData(
			data.scene_item_id, data.scene_item_group_id));
		if (idx != -1)
			ui->roiPropSceneItem->setCurrentIndex(idx);

		ui->roiPropSceneItemPadding->setValue((int)data.padding);

	} else if (item->type() == RoiListItem::Manual) {
		ui->roiPropertiesStack->setCurrentWidget(
			ui->roiManualPropertiesGroupBox);

		ui->roiPropPosX->setValue(data.posX);
		ui->roiPropPosY->setValue(data.posY);
		ui->roiPropSizeX->setValue(data.width);
		ui->roiPropSizeY->setValue(data.height);

	} else if (item->type() == RoiListItem::CenterFocus) {
		ui->roiPropertiesStack->setCurrentWidget(
			ui->roiCenterFocusPropertiesGroupBox);

		ui->roiPropOuterPrioritySlider->setValue(
			(int)(100 * data.outer_priority));
		ui->roiPropRadiusInnerSb->setValue(data.inner_radius);
		ui->roiPropRadiusInnerAspect->setChecked(data.inner_aspect);
		ui->roiPropRadiusInnerCircle->setChecked(data.inner_circle);
		ui->roiPropRadiusOuterSb->setValue(data.outer_radius);
		ui->roiPropRadiusOuterAspect->setChecked(data.outer_aspect);
		ui->roiPropStepsInnerSb->setValue(data.inner_steps);
		ui->roiPropStepsOuterSb->setValue(data.outer_steps);
		ui->roiPropCenterPosX->setValue(data.center_x);
		ui->roiPropCenterPosY->setValue(data.center_y);
	}

	/* Only set after loading so any signals to PropertiesChanged are no-ops */
	currentItem = new_item;
}

void RoiEditor::UpdatePreview()
{
	if (!isVisible())
		return;

	const uint32_t blockSize = ui->roiBlockSize->currentData().toInt();
	if (!blockSize)
		return;

	auto var = ui->sceneSelect->currentData();
	if (!var.isValid())
		return;

	const string scene_uuid = var.toString().toStdString();

	preview_roi_mutex.lock();
	preview_roi = RegionsFromData(scene_uuid);
	preview_outlines = RegionOutlinesFromData(scene_uuid);
	preview_roi_mutex.unlock();

	QString usage = QString("%1 %2")
				.arg(obs_module_text("ROI.Usage"))
				.arg(preview_roi.size());
	ui->roiWarningLabel->setVisible(preview_roi.size() > 256);
	ui->roiUsageLabel->setText(usage);

	OBSSourceAutoRelease program = obs_frontend_get_current_scene();
	const char *program_uuid = obs_source_get_uuid(program);

	if (scene_uuid != program_uuid) {
		OBSSourceAutoRelease selected =
			obs_get_source_by_uuid(scene_uuid.c_str());
		previewSource = obs_source_get_weak_source(selected);
	} else {
		previewSource = nullptr;
	}

	texBlockSize = blockSize;
	texOpacity = ui->previewOpacity->value();
	rebuild_texture = true;
}

/*
 * Region<->Data Conversions
 */

/* Resolve the current source name for a scene item region so the list shows
 * something readable even for regions saved by older versions. */
static QString ResolveSceneItemName(obs_source_t *scene_source,
				    int64_t item_id, int64_t group_id)
{
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene)
		return QString();

	obs_sceneitem_t *group = nullptr;
	if (group_id >= 0) {
		group = obs_scene_find_sceneitem_by_id(scene, group_id);
		if (!group || !obs_sceneitem_is_group(group))
			return QString();
		scene = obs_sceneitem_group_get_scene(group);
	}

	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, item_id);
	if (!item)
		return QString();

	QString name =
		QT_UTF8(obs_source_get_name(obs_sceneitem_get_source(item)));
	if (group)
		name = QString("%1 → %2").arg(
			QT_UTF8(obs_source_get_name(
				obs_sceneitem_get_source(group))),
			name);

	return name;
}

void RoiEditor::RegionItemsFromData()
{
	auto var = ui->sceneSelect->currentData();
	if (!var.isValid())
		return;

	ui->roiList->clear();

	const string scene_uuid = var.toString().toStdString();
	OBSSourceAutoRelease scene_source =
		obs_get_source_by_uuid(scene_uuid.c_str());

	for (obs_data_t *roi : roi_data[scene_uuid]) {
		int type = obs_data_get_int(roi, "type");
		RoiData data = RoiData::fromObsData(roi);

		if (type == RoiListItem::SceneItem && scene_source) {
			QString name = ResolveSceneItemName(
				scene_source, data.scene_item_id,
				data.scene_item_group_id);
			if (!name.isEmpty())
				data.scene_item_name = name;
		}

		RoiListItem *item = new RoiListItem(type);
		item->setData(ROIData, QVariant::fromValue<RoiData>(data));

		ui->roiList->addItem(item);
	}
}

void RoiEditor::RegionItemsToData()
{
	auto var = ui->sceneSelect->currentData();
	if (!var.isValid())
		return;

	const string scene_uuid = var.toString().toStdString();
	roi_data[scene_uuid].clear();

	int count = ui->roiList->count();
	for (int idx = 0; idx < count; idx++) {
		auto item = dynamic_cast<RoiListItem *>(ui->roiList->item(idx));
		if (!item)
			continue;

		auto var = item->data(ROIData);
		auto roi = var.value<RoiData>();

		obs_data_t *data = obs_data_create();
		obs_data_set_double(data, "priority", roi.priority);
		obs_data_set_bool(data, "enabled", roi.enabled);
		obs_data_set_int(data, "type", item->type());

		if (roi.per_codec_priority) {
			obs_data_set_bool(data, "per_codec_priority", true);
			obs_data_set_double(data, "priority_h264",
					    roi.priority_h264);
			obs_data_set_double(data, "priority_hevc",
					    roi.priority_hevc);
			obs_data_set_double(data, "priority_av1",
					    roi.priority_av1);
		}

		if (item->type() == RoiListItem::SceneItem) {
			obs_data_set_string(
				data, "scene_item_name",
				roi.scene_item_name.toUtf8().constData());
			obs_data_set_int(data, "scene_item_id",
					 roi.scene_item_id);
			if (roi.scene_item_group_id >= 0)
				obs_data_set_int(data, "scene_item_group_id",
						 roi.scene_item_group_id);
			if (roi.padding > 0)
				obs_data_set_int(data, "padding", roi.padding);
			obs_data_set_int(data, "smoothing_steps",
					 roi.smoothing_steps);
			obs_data_set_int(data, "smoothing_type",
					 roi.smoothing_type);
			obs_data_set_double(data, "smoothing_priority",
					    roi.smoothing_priority);
		} else if (item->type() == RoiListItem::Manual) {
			obs_data_set_int(data, "x", roi.posX);
			obs_data_set_int(data, "y", roi.posY);
			obs_data_set_int(data, "width", roi.width);
			obs_data_set_int(data, "height", roi.height);
			obs_data_set_int(data, "smoothing_steps",
					 roi.smoothing_steps);
			obs_data_set_double(data, "smoothing_priority",
					    roi.smoothing_priority);
			obs_data_set_int(data, "smoothing_type",
					 roi.smoothing_type);
		} else if (item->type() == RoiListItem::CenterFocus) {
			obs_data_set_int(data, "center_radius_inner",
					 roi.inner_radius);
			obs_data_set_int(data, "center_radius_outer",
					 roi.outer_radius);
			obs_data_set_bool(data, "center_aspect_inner",
					  roi.inner_aspect);
			obs_data_set_bool(data, "center_circle",
					  roi.inner_circle);
			obs_data_set_bool(data, "center_aspect_outer",
					  roi.outer_aspect);
			obs_data_set_int(data, "center_steps_inner",
					 roi.inner_steps);
			obs_data_set_int(data, "center_steps_outer",
					 roi.outer_steps);
			obs_data_set_double(data, "center_priority_outer",
					    roi.outer_priority);
			obs_data_set_int(data, "center_x", roi.center_x);
			obs_data_set_double(data, "center_y", roi.center_y);
		}

		roi_data[scene_uuid].emplace_back(data);
	}
}

static obs_encoder_roi GetItemROI(obs_sceneitem_t *item,
				  obs_sceneitem_t *group, float priority,
				  int64_t padding)
{
	obs_encoder_roi roi;

	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	/* Items inside a group are positioned relative to the group, so
	 * compose with the group's transform to get canvas coordinates. */
	if (group) {
		matrix4 groupTransform;
		obs_sceneitem_get_draw_transform(group, &groupTransform);
		matrix4_mul(&boxTransform, &boxTransform, &groupTransform);
	}

	vec3 tl, br;
	vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
	vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);

	auto GetMinPos = [&](float x, float y) {
		vec3 pos;
		vec3_set(&pos, x, y, 0.0f);
		vec3_transform(&pos, &pos, &boxTransform);
		vec3_min(&tl, &tl, &pos);
		vec3_max(&br, &br, &pos);
	};

	GetMinPos(0.0f, 0.0f);
	GetMinPos(1.0f, 0.0f);
	GetMinPos(0.0f, 1.0f);
	GetMinPos(1.0f, 1.0f);

	const float pad = static_cast<float>(padding);

	roi.left = static_cast<uint32_t>(std::max(tl.x - pad, 0.0f));
	roi.top = static_cast<uint32_t>(std::max(tl.y - pad, 0.0f));
	roi.right = static_cast<uint32_t>(std::max(br.x + pad, 0.0f));
	roi.bottom = static_cast<uint32_t>(std::max(br.y + pad, 0.0f));
	roi.priority = priority;

	/* Negative padding may shrink the region into nothing; collapse it
	 * instead of letting the edges cross (the resulting region is
	 * rejected as too small rather than wrapping around). */
	if (roi.right < roi.left)
		roi.right = roi.left;
	if (roi.bottom < roi.top)
		roi.bottom = roi.top;

	return roi;
}

static constexpr int32_t kMinBlockSize = 16; // Use H.264 as a baseline

static void BuildInnerRegions(vector<obs_encoder_roi> &rois, float priority,
			      int64_t steps, int64_t radius,
			      bool correct_aspect, int32_t center_x,
			      int32_t center_y, bool circle_inner,
			      uint32_t width, uint32_t height)
{
	if (!radius || height < radius || width < radius ||
	    radius < kMinBlockSize / 2 || priority == 0.0 || !steps)
		return;
	int32_t interval = radius / steps;

	if (interval < kMinBlockSize) {
		/* Clamp interval size and step count to the smallest block size */
		interval = kMinBlockSize;
		steps = std::max(radius / interval, 1LL);
	} else if (interval % kMinBlockSize) {
		/* Round interval to nearest multiple of kMinBlockSize */
		interval =
			(int32_t)round((float)interval / float(kMinBlockSize)) *
			kMinBlockSize;
		steps = std::max((radius + kMinBlockSize) / interval, 1LL);
	}

	double priority_interval = priority / (double)steps;
	double aspect = 1.0;
	if (correct_aspect)
		aspect = (double)width / (double)height;

	int32_t middle_x = center_x >= 0 ? center_x : width / 2;
	int32_t middle_y = center_y >= 0 ? center_y : height / 2;

	if (!circle_inner) {
		for (int32_t i = 1; i <= steps; i++) {
			// Configurable center point means we have to clamp these.
			uint32_t top =
				std::clamp(middle_y - interval * i, 0, 16384);
			uint32_t bottom =
				std::clamp(middle_y + interval * i, 0, 16384);
			uint32_t left = std::clamp(
				(int32_t)(middle_x - interval * i * aspect), 0,
				16384);
			uint32_t right = std::clamp(
				(int32_t)(middle_x + interval * i * aspect), 0,
				16384);
			float region_priority =
				(float)(priority - priority_interval * (i - 1));

			obs_encoder_roi roi = {top, bottom, left, right,
					       region_priority};
			rois.push_back(roi);
		}
	} else {
		// Circular region, extremely inefficient.
		for (int32_t i = 1; i <= steps; i++) {
			float region_priority =
				(float)(priority - priority_interval * (i - 1));

			int32_t radius = interval * i;
			int32_t x_off = kMinBlockSize / 2;
			int32_t prev_y_off = 0;

			while (x_off < radius) {
				int32_t y_off =
					sqrt(pow(radius, 2) - pow(x_off, 2));
				if (y_off <= 0)
					break;

				// Avoid overlapping/duplicate regions
				if (y_off != prev_y_off) {
					obs_encoder_roi roi = {
						(uint32_t)(middle_y - y_off),
						(uint32_t)(middle_y + y_off),
						(uint32_t)(middle_x -
							   x_off * aspect),
						(uint32_t)(middle_x +
							   x_off * aspect),
						region_priority};
					rois.push_back(roi);
					prev_y_off = y_off;
				}

				x_off += kMinBlockSize / 2;
			}
		}
	}
}

static void BuildOuterRegions(vector<obs_encoder_roi> &rois, float priority,
			      int64_t steps, int64_t radius,
			      bool correct_aspect, uint32_t width,
			      uint32_t height)
{
	if (!radius || height / 2 < radius || width / 2 < radius ||
	    radius < kMinBlockSize || priority == 0.0 || !steps)
		return;

	int64_t interval = radius / steps;

	if (interval < kMinBlockSize) {
		/* Clamp interval size and step count to the smallest block size */
		interval = kMinBlockSize;
		steps = std::max(radius / interval, 1LL);
	} else if (interval % kMinBlockSize) {
		/* Round interval to nearest multiple of kMinBlockSize */
		interval =
			(int64_t)round((float)interval / float(kMinBlockSize)) *
			kMinBlockSize;
		steps = std::max((radius + kMinBlockSize) / interval, 1LL);
	}

	double priority_interval = priority / (double)steps;
	double aspect = 1.0;
	if (correct_aspect)
		aspect = (double)width / (double)height;

	/* Add neutral baseline */
	obs_encoder_roi neutral = {(uint32_t)radius,
				   (uint32_t)(height - radius),
				   (uint32_t)((double)radius * aspect),
				   (uint32_t)(width - (double)radius * aspect),
				   0.0f};
	rois.push_back(neutral);

	for (int i = 1; steps > 1 && i < steps; i++) {
		obs_encoder_roi roi = {
			(uint32_t)(radius - interval * i),
			(uint32_t)(height - radius + interval * i),
			(uint32_t)((double)(radius - interval * i) * aspect),
			(uint32_t)(width -
				   (double)(radius - interval * i) * aspect),
			(float)(priority_interval * i)};
		rois.push_back(roi);
	}

	/* Ensure last region always goes to frame edges */
	obs_encoder_roi final = {0, height, 0, width, (float)priority};
	rois.push_back(final);
}

static void BuildCenterFocusROI(vector<obs_encoder_roi> &rois, obs_data_t *data,
				uint32_t width, uint32_t height,
				double priority)
{
	int64_t inner_radius = obs_data_get_int(data, "center_radius_inner");
	bool aspect_inner = obs_data_get_bool(data, "center_aspect_inner");
	bool circle_inner = obs_data_get_bool(data, "center_circle");
	int64_t outer_radius = obs_data_get_int(data, "center_radius_outer");
	bool aspect_outer = obs_data_get_bool(data, "center_aspect_outer");
	int64_t steps_inner = obs_data_get_int(data, "center_steps_inner");
	int64_t steps_outer = obs_data_get_int(data, "center_steps_outer");
	int32_t center_x = obs_data_get_int(data, "center_x");
	int32_t center_y = obs_data_get_int(data, "center_y");
	double priority_outer =
		obs_data_get_double(data, "center_priority_outer");

	/* Inner regions (if any) */
	BuildInnerRegions(rois, priority, steps_inner, inner_radius,
			  aspect_inner, center_x, center_y, circle_inner, width,
			  height);
	BuildOuterRegions(rois, priority_outer, steps_outer, outer_radius,
			  aspect_outer, width, height);
}

/// Split specified ROI up into multiple based on given mode
static void SmoothROI(vector<obs_encoder_roi> &regions,
		      const obs_encoder_roi &roi, RoiEditor::Smoothing type,
		      int steps, const double edge_priority)
{
	int max_steps = 0;
	uint32_t width = roi.right - roi.left;
	uint32_t height = roi.bottom - roi.top;

	// Figure out how many steps we can even do
	if (type == RoiEditor::Inside) {
		max_steps = std::min(width / kMinBlockSize / 2,
				     height / kMinBlockSize / 2);
	} else if (type == RoiEditor::Outside) {
		max_steps = 64; // limit to something reasonable
	} else if (type == RoiEditor::Edge) {
		// Effectively gives us inside + outside
		max_steps = std::min(width / kMinBlockSize + 1,
				     height / kMinBlockSize + 1);
	}

	steps = std::min(steps, max_steps);

	if (type == RoiEditor::None || steps < 2) {
		regions.push_back(roi);
		return;
	}

	// Just create a bunch of additional zones fading to outside priority
	double interval = (roi.priority - edge_priority) / (double)(steps - 1);

	int32_t step_offset = 0;
	if (type == RoiEditor::Edge)
		step_offset = -steps / 2 + 1;
	else if (type == RoiEditor::Inside)
		step_offset = -steps + 1;

	for (int32_t step = 0; step < steps; step++) {
		float region_priority = std::clamp(
			roi.priority - (float)(interval * step), -1.0f, 1.0f);

		int32_t mul = step + step_offset;
		uint32_t top = std::clamp(
			(int32_t)roi.top - kMinBlockSize * mul, 0, 16384);
		uint32_t bottom = std::clamp(
			(int32_t)roi.bottom + kMinBlockSize * mul, 0, 16384);
		uint32_t left = std::clamp(
			(int32_t)roi.left - kMinBlockSize * mul, 0, 16384);
		uint32_t right = std::clamp(
			(int32_t)roi.right + kMinBlockSize * mul, 0, 16384);

		obs_encoder_roi step_region = {
			top, bottom, left, right, region_priority,
		};
		regions.push_back(step_region);
	}
}

/// Find the scene item (and parent group) a region targets; returns whether
/// the item exists and is visible.
static bool ResolveSceneItemRegion(obs_source_t *source, obs_data_t *data,
				   OBSSceneItem &sceneItem,
				   OBSSceneItem &groupItem, int64_t &padding)
{
	int64_t id = obs_data_get_int(data, "scene_item_id");
	int64_t group_id =
		obs_data_has_user_value(data, "scene_item_group_id")
			? obs_data_get_int(data, "scene_item_group_id")
			: -1;
	padding = obs_data_get_int(data, "padding");

	if (group_id >= 0) {
		groupItem = obs_scene_find_sceneitem_by_id(
			obs_scene_from_source(source), group_id);
		if (!groupItem || !obs_sceneitem_is_group(groupItem))
			return false;

		sceneItem = obs_scene_find_sceneitem_by_id(
			obs_sceneitem_group_get_scene(groupItem), id);
		return sceneItem && obs_sceneitem_visible(groupItem) &&
		       obs_sceneitem_visible(sceneItem);
	}

	sceneItem = obs_scene_find_sceneitem_by_id(
		obs_scene_from_source(source), id);
	return sceneItem && obs_sceneitem_visible(sceneItem);
}

/// Exact (unsnapped) rectangles of enabled regions, for preview outlines.
/// Center focus regions are skipped as they consist of many stacked rings.
vector<obs_encoder_roi> RoiEditor::RegionOutlinesFromData(const string &uuid)
{
	const auto &region_data = roi_data[uuid];
	if (region_data.empty())
		return {};
	OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
	if (!source)
		return {};

	vector<obs_encoder_roi> outlines;

	for (obs_data_t *data : region_data) {
		if (!obs_data_get_bool(data, "enabled"))
			continue;

		auto type = static_cast<RoiListItem::RoiItemType>(
			obs_data_get_int(data, "type"));

		if (type == RoiListItem::SceneItem) {
			OBSSceneItem sceneItem;
			OBSSceneItem groupItem;
			int64_t padding;
			if (!ResolveSceneItemRegion(source, data, sceneItem,
						    groupItem, padding))
				continue;

			auto roi = GetItemROI(
				sceneItem, groupItem,
				(float)obs_data_get_double(data, "priority"),
				padding);
			if (roi.bottom && roi.right)
				outlines.push_back(roi);
		} else if (type == RoiListItem::Manual) {
			uint32_t left = (uint32_t)obs_data_get_int(data, "x");
			uint32_t top = (uint32_t)obs_data_get_int(data, "y");
			uint32_t right = left + (uint32_t)obs_data_get_int(
							data, "width");
			uint32_t bottom = top + (uint32_t)obs_data_get_int(
							data, "height");
			if (right && bottom)
				outlines.push_back(
					{top, bottom, left, right, 0.0f});
		}
	}

	return outlines;
}

/* Priority of a region for the given codec, falling back to the region's
 * base priority unless per-codec customization is enabled. */
static float RegionPriorityForCodec(obs_data_t *data, const char *codec)
{
	const float base = (float)obs_data_get_double(data, "priority");

	if (!codec || !obs_data_get_bool(data, "per_codec_priority"))
		return base;

	if (strcmp(codec, "h264") == 0)
		return (float)obs_data_get_double(data, "priority_h264");
	if (strcmp(codec, "hevc") == 0)
		return (float)obs_data_get_double(data, "priority_hevc");
	if (strcmp(codec, "av1") == 0)
		return (float)obs_data_get_double(data, "priority_av1");

	return base;
}

/// Create actual obs_encoder_roi structs from configured regions
vector<obs_encoder_roi> RoiEditor::RegionsFromData(const string &uuid,
						   const char *codec)
{
	const auto &region_data = roi_data[uuid];
	if (region_data.empty())
		return {};
	OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
	if (!source)
		return {};

	vector<obs_encoder_roi> regions;

	for (obs_data_t *data : region_data) {
		float priority = RegionPriorityForCodec(data, codec);

		if (!obs_data_get_bool(data, "enabled"))
			continue;

		auto type = static_cast<RoiListItem::RoiItemType>(
			obs_data_get_int(data, "type"));
		auto smoothing_type = static_cast<RoiEditor::Smoothing>(
			obs_data_get_int(data, "smoothing_type"));
		int smoothing_steps = obs_data_get_int(data, "smoothing_steps");
		double smoothing_priority =
			obs_data_get_double(data, "smoothing_priority");

		if (type == RoiListItem::SceneItem) {
			/* Scene Item ROI */
			OBSSceneItem sceneItem;
			OBSSceneItem groupItem;
			int64_t padding;
			bool visible = ResolveSceneItemRegion(
				source, data, sceneItem, groupItem, padding);

			if (visible) {
				auto roi = GetItemROI(sceneItem, groupItem,
						      priority, padding);
				if (roi.bottom == 0 || roi.right == 0)
					continue;

				if (smoothing_type != Smoothing::None &&
				    smoothing_steps > 1 &&
				    smoothing_priority != priority) {
					SmoothROI(regions, roi, smoothing_type,
						  smoothing_steps,
						  smoothing_priority);
				} else {
					regions.push_back(roi);
				}
			}

		} else if (type == RoiListItem::Manual) {
			/* Fixed ROI */
			uint32_t left = (uint32_t)obs_data_get_int(data, "x");
			uint32_t top = (uint32_t)obs_data_get_int(data, "y");
			uint32_t right = left + (uint32_t)obs_data_get_int(
							data, "width");
			uint32_t bottom = top + (uint32_t)obs_data_get_int(
							data, "height");

			// Invalid ROI
			if (right == 0 || bottom == 0)
				continue;

			obs_encoder_roi roi{top, bottom, left, right, priority};

			if (smoothing_type != Smoothing::None &&
			    smoothing_steps > 1 &&
			    smoothing_priority != priority) {
				SmoothROI(regions, roi, smoothing_type,
					  smoothing_steps, smoothing_priority);
			} else {
				regions.push_back(roi);
			}

		} else if (type == RoiListItem::CenterFocus) {
			/* Center-focus ROI */
			uint32_t cx = obs_source_get_width(source);
			uint32_t cy = obs_source_get_height(source);
			BuildCenterFocusROI(regions, data, cx, cy, priority);
		}
	}

	return regions;
}

/*
 * Public slots
 */

void RoiEditor::ShowHideDialog()
{
	if (!isVisible()) {
		setVisible(true);
		CreateDisplay(true);
		RefreshSceneList();
		RegionItemsFromData();
		RefreshData();

		if (!geometry.isEmpty())
			restoreGeometry(geometry);
	} else {
		close();
	}
}

void RoiEditor::ToggleRoiEnabled()
{
	ui->enableRoi->toggle();
}

/* Active scene + base→output scaling of one canvas (video mix) */
struct CanvasTarget {
	video_t *video;
	std::string scene_uuid;
	double scale_x;
	double scale_y;
};

static void CollectEncodersFromOutput(obs_output_t *output,
				      std::vector<OBSEncoder> &encoders)
{
	if (!output)
		return;

	for (size_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		obs_encoder_t *enc = obs_output_get_video_encoder2(output, idx);
		if (!enc)
			continue;
		if (!(obs_encoder_get_caps(enc) & OBS_ENCODER_CAP_ROI))
			continue;

		bool duplicate = false;
		for (obs_encoder_t *existing : encoders) {
			if (existing == enc) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			encoders.emplace_back(enc);
	}
}

void RoiEditor::UpdateEncoders()
{
	if (shuttingDown)
		return;

	/* Each canvas (main + e.g. a vertical canvas) has its own video mix,
	 * active scene, and base→output scaling. Encoders are matched to
	 * their canvas through the video mix they consume, so every output —
	 * including third-party ones such as a vertical canvas stream or
	 * multistream outputs, regardless of which GPU encodes them — gets
	 * the regions of the scene it is actually showing. */
	std::vector<CanvasTarget> targets;

	{
		OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
		obs_video_info ovi;
		if (scene && obs_get_video_info(&ovi) && ovi.base_width &&
		    ovi.base_height) {
			targets.push_back(
				{obs_get_video(),
				 obs_source_get_uuid(scene),
				 (double)ovi.output_width /
					 (double)ovi.base_width,
				 (double)ovi.output_height /
					 (double)ovi.base_height});
		}
	}

	OBSCanvasAutoRelease main_canvas = obs_get_main_canvas();
	struct CanvasEnumCtx {
		std::vector<CanvasTarget> *targets;
		obs_canvas_t *main;
	} canvas_ctx{&targets, main_canvas.Get()};

	obs_enum_canvases(
		[](void *param, obs_canvas_t *canvas) -> bool {
			auto ctx = static_cast<CanvasEnumCtx *>(param);
			if (canvas == ctx->main ||
			    !obs_canvas_has_video(canvas))
				return true;

			OBSSourceAutoRelease active =
				GetCanvasActiveScene(canvas);
			obs_video_info ovi;
			if (!active ||
			    !obs_canvas_get_video_info(canvas, &ovi) ||
			    !ovi.base_width || !ovi.base_height)
				return true;

			ctx->targets->push_back(
				{obs_canvas_get_video(canvas),
				 obs_source_get_uuid(active),
				 (double)ovi.output_width /
					 (double)ovi.base_width,
				 (double)ovi.output_height /
					 (double)ovi.base_height});
			return true;
		},
		&canvas_ctx);

	if (targets.empty())
		return;

	std::vector<OBSEncoder> encoders;

	if (!enumerate_all_encoders) {
		/* All outputs, including third-party ones (vertical canvas
		 * streams, multistream plugins, the encoder preview, ...) */
		obs_enum_outputs(
			[](void *param, obs_output_t *output) -> bool {
				CollectEncodersFromOutput(
					output,
					*static_cast<std::vector<OBSEncoder> *>(
						param));
				return true;
			},
			&encoders);

		/* Optionally drop encoders that only serve recording/replay */
		if (ui->excludeRecordings->isChecked()) {
			std::vector<OBSEncoder> excluded;
			OBSOutputAutoRelease rec =
				obs_frontend_get_recording_output();
			OBSOutputAutoRelease replay =
				obs_frontend_get_replay_buffer_output();
			CollectEncodersFromOutput(rec, excluded);
			CollectEncodersFromOutput(replay, excluded);

			std::vector<OBSEncoder> streaming;
			OBSOutputAutoRelease stream =
				obs_frontend_get_streaming_output();
			CollectEncodersFromOutput(stream, streaming);

			auto is_in = [](const std::vector<OBSEncoder> &vec,
					obs_encoder_t *enc) {
				for (obs_encoder_t *e : vec) {
					if (e == enc)
						return true;
				}
				return false;
			};

			encoders.erase(
				std::remove_if(
					encoders.begin(), encoders.end(),
					[&](const OBSEncoder &enc) {
						return is_in(excluded, enc) &&
						       !is_in(streaming, enc);
					}),
				encoders.end());
		}
	} else {
		// Alternative more thorough option for special cases
		auto cb = [](void *param, obs_encoder_t *enc) {
			auto vec =
				static_cast<std::vector<OBSEncoder> *>(param);

			if (obs_encoder_get_type(enc) == OBS_ENCODER_VIDEO)
				vec->push_back(enc);

			return true;
		};

		obs_enum_encoders(cb, &encoders);
	}

	if (encoders.empty()) {
		SetStatusLabel({});
		return;
	}

	// Clear any ROIs that might exist
	for (obs_encoder_t *enc : encoders)
		obs_encoder_clear_roi(enc);

	if (!ui->enableRoi->isChecked()) {
		SetStatusLabel({});
		return;
	}

	/* Regions per (scene, codec), in the canvas's output resolution.
	 * Regions can carry per-codec priorities, so the sets are built per
	 * codec on demand. libobs handles further scaling for rescaled
	 * encoders such as the Enhanced Broadcasting/multitrack tracks. */
	std::unordered_map<std::string, std::vector<obs_encoder_roi>>
		region_cache;

	auto regionsFor = [&](const CanvasTarget &target,
			      const char *codec)
		-> const std::vector<obs_encoder_roi> & {
		std::string key = target.scene_uuid;
		key += "|";
		key += codec ? codec : "";

		auto it = region_cache.find(key);
		if (it != region_cache.end())
			return it->second;

		std::vector<obs_encoder_roi> regions;
		if (roi_data.count(target.scene_uuid)) {
			regions = RegionsFromData(target.scene_uuid, codec);

			if (target.scale_x != 1.0 || target.scale_y != 1.0) {
				for (obs_encoder_roi &roi : regions) {
					roi.top = (uint32_t)((double)roi.top *
							     target.scale_y);
					roi.bottom =
						(uint32_t)((double)roi.bottom *
							   target.scale_y);
					roi.left = (uint32_t)((double)roi.left *
							      target.scale_x);
					roi.right =
						(uint32_t)((double)roi.right *
							   target.scale_x);
				}
			}
		}

		return region_cache.emplace(key, std::move(regions))
			.first->second;
	};

	for (obs_encoder_t *enc : encoders) {
		/* We might have already set the ROI (e.g. shared streaming/recording encoder) */
		if (obs_encoder_has_roi(enc))
			continue;

		/* Match the encoder to its canvas via the video mix it
		 * consumes; unmatched encoders fall back to the main canvas. */
		video_t *enc_video = obs_encoder_video(enc);
		const CanvasTarget *target = &targets.front();
		for (const CanvasTarget &candidate : targets) {
			if (candidate.video == enc_video) {
				target = &candidate;
				break;
			}
		}

		const char *codec = obs_encoder_get_codec(enc);
		const auto &regions = regionsFor(*target, codec);
		if (regions.empty())
			continue;

		blog(LOG_INFO, "Adding ROI to encoder: %s",
		     obs_encoder_get_name(enc));

		for (const obs_encoder_roi &roi : regions)
			obs_encoder_add_roi(enc, &roi);
	}

	QStringList applied;
	for (obs_encoder_t *enc : encoders) {
		if (obs_encoder_has_roi(enc))
			applied << QT_UTF8(obs_encoder_get_name(enc));
	}
	SetStatusLabel(applied);
}

void RoiEditor::SetStatusLabel(const QStringList &encoder_names)
{
	if (encoder_names.isEmpty()) {
		ui->roiStatusLabel->setText(
			obs_module_text("ROI.Status.None"));
	} else {
		ui->roiStatusLabel->setText(
			QString(obs_module_text("ROI.Status.Applied"))
				.arg(encoder_names.size())
				.arg(encoder_names.join(", ")));
	}
}

/*
 * Toolbar actions
 */

void RoiEditor::AddRegionItem(int type)
{
	auto item = new RoiListItem(type);
	RoiData roi = {};
	roi.enabled = true;
	item->setData(ROIData, QVariant::fromValue<RoiData>(roi));

	ui->roiList->insertItem(0, item);
	ui->roiList->setCurrentItem(item);

	RefreshData();
}

void RoiEditor::AddBackgroundItem()
{
	UpdateEditCanvasSize();
	const uint32_t base_width = editCanvasWidth;
	const uint32_t base_height = editCanvasHeight;
	if (!base_width || !base_height)
		return;

	auto item = new RoiListItem(RoiListItem::Manual);
	RoiData roi = {};
	roi.enabled = true;
	roi.priority = -0.25f;
	roi.posX = 0;
	roi.posY = 0;
	roi.width = base_width;
	roi.height = base_height;
	item->setData(ROIData, QVariant::fromValue<RoiData>(roi));

	/* Insert at the bottom so explicit regions above it win on overlap. */
	ui->roiList->addItem(item);
	ui->roiList->setCurrentItem(item);

	RefreshData();
}

void RoiEditor::on_actionAddRoi_triggered()
{
	auto popup = QMenu(obs_module_text("ROI.AddMenu"), this);

	QAction *addSceneItemRoi =
		new QAction(obs_module_text("ROI.AddMenu.SceneItem"), this);
	QAction *addManualRoi =
		new QAction(obs_module_text("ROI.AddMenu.Manual"), this);
	QAction *addCenterRoi =
		new QAction(obs_module_text("ROI.AddMenu.CenterFocus"), this);
	QAction *addBackgroundRoi =
		new QAction(obs_module_text("ROI.AddMenu.Background"), this);

	connect(addSceneItemRoi, &QAction::triggered,
		[this] { AddRegionItem(RoiListItem::SceneItem); });
	connect(addManualRoi, &QAction::triggered,
		[this] { AddRegionItem(RoiListItem::Manual); });
	connect(addCenterRoi, &QAction::triggered,
		[this] { AddRegionItem(RoiListItem::CenterFocus); });
	connect(addBackgroundRoi, &QAction::triggered,
		[this] { AddBackgroundItem(); });

	popup.insertAction(nullptr, addSceneItemRoi);
	popup.insertAction(addSceneItemRoi, addManualRoi);
	popup.insertAction(addManualRoi, addCenterRoi);
	popup.insertAction(addCenterRoi, addBackgroundRoi);

	popup.exec(QCursor::pos());
}

void RoiEditor::on_actionCopyRegions_triggered()
{
	auto var = ui->sceneSelect->currentData();
	if (!var.isValid())
		return;

	const string current_uuid = var.toString().toStdString();

	/* Make sure the stored data reflects the current UI state */
	RegionItemsToData();

	QMenu popup(obs_module_text("ROI.CopyMenu"), this);

	QAction *allScenes =
		popup.addAction(obs_module_text("ROI.CopyMenu.AllScenes"));
	allScenes->setData(QString());
	popup.addSeparator();

	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);

	for (size_t idx = 0; idx < scenes.sources.num; idx++) {
		obs_source_t *src = scenes.sources.array[idx];
		const char *uuid = obs_source_get_uuid(src);
		if (current_uuid == uuid)
			continue;

		QAction *action =
			popup.addAction(QT_UTF8(obs_source_get_name(src)));
		action->setData(QString(uuid));
	}

	obs_frontend_source_list_free(&scenes);

	QAction *chosen = popup.exec(QCursor::pos());
	if (!chosen)
		return;

	auto copyTo = [&](const string &target_uuid) {
		if (target_uuid == current_uuid)
			return;

		auto &dst = roi_data[target_uuid];
		dst.clear();
		for (obs_data_t *region : roi_data[current_uuid]) {
			dst.emplace_back(obs_data_create_from_json(
				obs_data_get_json(region)));
		}
	};

	QString selected = chosen->data().toString();
	if (selected.isEmpty()) {
		obs_frontend_source_list all = {};
		obs_frontend_get_scenes(&all);
		for (size_t idx = 0; idx < all.sources.num; idx++) {
			copyTo(obs_source_get_uuid(all.sources.array[idx]));
		}
		obs_frontend_source_list_free(&all);
	} else {
		copyTo(selected.toStdString());
	}

	obs_frontend_save();
	UpdateEncoders();
}

void RoiEditor::on_actionExportRegions_triggered()
{
	auto var = ui->sceneSelect->currentData();
	if (!var.isValid())
		return;

	RegionItemsToData();
	const string uuid = var.toString().toStdString();

	QString path = QFileDialog::getSaveFileName(
		this, obs_module_text("ROI.Export"), QString(),
		"JSON (*.json)");
	if (path.isEmpty())
		return;

	OBSDataAutoRelease root = obs_data_create();
	OBSDataArrayAutoRelease arr = obs_data_array_create();

	for (obs_data_t *region : roi_data[uuid])
		obs_data_array_push_back(arr, region);

	obs_data_set_array(root, "regions", arr);

	if (!obs_data_save_json(root, QT_TO_UTF8(path))) {
		QMessageBox::warning(this, obs_module_text("ROI.Export"),
				     obs_module_text("ROI.Export.Failed"));
	}
}

void RoiEditor::on_actionImportRegions_triggered()
{
	auto var = ui->sceneSelect->currentData();
	if (!var.isValid())
		return;

	const string uuid = var.toString().toStdString();

	QString path = QFileDialog::getOpenFileName(
		this, obs_module_text("ROI.Import"), QString(),
		"JSON (*.json)");
	if (path.isEmpty())
		return;

	OBSDataAutoRelease root =
		obs_data_create_from_json_file(QT_TO_UTF8(path));
	OBSDataArrayAutoRelease arr =
		root ? obs_data_get_array(root, "regions") : nullptr;
	size_t count = arr ? obs_data_array_count(arr) : 0;

	if (!count) {
		QMessageBox::warning(this, obs_module_text("ROI.Import"),
				     obs_module_text("ROI.Import.Failed"));
		return;
	}

	auto &dst = roi_data[uuid];
	dst.clear();
	for (size_t idx = 0; idx < count; idx++)
		dst.emplace_back(obs_data_array_item(arr, idx));

	RegionItemsFromData();
	RefreshData();
	obs_frontend_save();
}

void RoiEditor::on_actionRemoveRoi_triggered()
{
	if (!currentItem)
		return;

	RoiListItem *item = currentItem;
	currentItem = nullptr;
	delete item;

	RefreshData();
}

void RoiEditor::MoveRoiItem(Direction direction)
{
	int idx = ui->roiList->currentRow();
	if (idx == -1)
		return;
	if (idx == 0 && direction == Up)
		return;
	if (idx == ui->roiList->count() - 1 && direction == Down)
		return;

	QSignalBlocker sb(ui->roiList);

	QListWidgetItem *item = ui->roiList->takeItem(idx);

	int offset = direction == Up ? -1 : 1;
	ui->roiList->insertItem(idx + offset, item);
	ui->roiList->setCurrentRow(idx + offset);
	item->setSelected(true);
}

void RoiEditor::on_actionRoiUp_triggered()
{
	MoveRoiItem(Direction::Up);
	RefreshData();
}
void RoiEditor::on_actionRoiDown_triggered()
{
	MoveRoiItem(Direction::Down);
	RefreshData();
}

/*
 * Signal handling
 */

void RoiEditor::SceneItemChanged(void *param, calldata_t *)
{
	RoiEditor *window = reinterpret_cast<RoiEditor *>(param);
	if (window->shuttingDown)
		return;
	QMetaObject::invokeMethod(window, "UpdatePreview");
	QMetaObject::invokeMethod(window, "UpdateEncoders");
}

void RoiEditor::ItemRemovedOrAdded(void *param, calldata_t *)
{
	RoiEditor *window = reinterpret_cast<RoiEditor *>(param);
	if (window->shuttingDown)
		return;
	// The "item_remove" signal comes in before the item is actually removed,
	// so defer the refresh to avoid getting the list from libobs before it is updated.
	QMetaObject::invokeMethod(window, "RefreshSceneItems",
				  Qt::QueuedConnection);
}

void RoiEditor::CanvasChannelChanged(void *param, calldata_t *)
{
	RoiEditor *window = reinterpret_cast<RoiEditor *>(param);
	if (window->shuttingDown)
		return;
	/* Deferred: reconnecting signals would disconnect the handler that is
	 * currently executing. */
	QMetaObject::invokeMethod(
		window,
		[window]() {
			window->ConnectSceneSignals();
			window->UpdateEncoders();
		},
		Qt::QueuedConnection);
}

/* Disconnect all libobs signal connections while their owners are still
 * alive, and refuse any later reconnection/encoder updates. Late callbacks
 * (e.g. canvas channel_change firing while OBS clears scene data) would
 * otherwise touch destroyed signal handlers. */
void RoiEditor::PrepareShutdown()
{
	shuttingDown = true;
	sceneSignals.clear();
}

void RoiEditor::ConnectSceneSignals()
{
	if (shuttingDown)
		return;

	sceneSignals.clear();

	OBSSourceAutoRelease source = obs_frontend_get_current_scene();
	if (source)
		ConnectSignalsForScene(source);

	/* Track scene switches and item changes on additional canvases */
	OBSCanvasAutoRelease main_canvas = obs_get_main_canvas();
	struct CanvasSignalCtx {
		RoiEditor *editor;
		obs_canvas_t *main;
	} ctx{this, main_canvas.Get()};

	obs_enum_canvases(
		[](void *param, obs_canvas_t *canvas) -> bool {
			auto ctx = static_cast<CanvasSignalCtx *>(param);
			if (canvas == ctx->main)
				return true;

			signal_handler_t *canvas_signal =
				obs_canvas_get_signal_handler(canvas);
			if (canvas_signal)
				ctx->editor->sceneSignals.emplace_back(
					canvas_signal, "channel_change",
					CanvasChannelChanged, ctx->editor);

			/* If a transition (e.g. a stinger) sits on channel 0,
			 * scene switches fire transition_stop rather than
			 * channel_change, so listen for that too. */
			OBSSourceAutoRelease channel =
				obs_canvas_get_channel(canvas, 0);
			if (channel &&
			    obs_source_get_type(channel) ==
				    OBS_SOURCE_TYPE_TRANSITION) {
				signal_handler_t *tsh =
					obs_source_get_signal_handler(channel);
				if (tsh)
					ctx->editor->sceneSignals.emplace_back(
						tsh, "transition_stop",
						CanvasChannelChanged,
						ctx->editor);
			}

			OBSSourceAutoRelease active =
				GetCanvasActiveScene(canvas);
			if (active)
				ctx->editor->ConnectSignalsForScene(active);

			return true;
		},
		&ctx);
}

void RoiEditor::ConnectSignalsForScene(obs_source_t *source)
{
	signal_handler_t *signal = obs_source_get_signal_handler(source);
	if (!signal)
		return;

	sceneSignals.emplace_back(signal, "item_transform", SceneItemChanged,
				  this);
	sceneSignals.emplace_back(signal, "item_visible", SceneItemChanged,
				  this);
	sceneSignals.emplace_back(signal, "item_add", ItemRemovedOrAdded, this);
	sceneSignals.emplace_back(signal, "item_remove", ItemRemovedOrAdded,
				  this);
	sceneSignals.emplace_back(signal, "refresh", ItemRemovedOrAdded, this);

	/* Items inside groups emit their signals on the group's sub-scene,
	 * so connect those as well to track changes within groups. */
	obs_scene_enum_items(
		obs_scene_from_source(source),
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) -> bool {
			if (!obs_sceneitem_is_group(item))
				return true;

			auto editor = static_cast<RoiEditor *>(param);
			obs_scene_t *group_scene =
				obs_sceneitem_group_get_scene(item);
			signal_handler_t *group_signal =
				obs_source_get_signal_handler(
					obs_scene_get_source(group_scene));
			if (!group_signal)
				return true;

			editor->sceneSignals.emplace_back(group_signal,
							  "item_transform",
							  SceneItemChanged,
							  editor);
			editor->sceneSignals.emplace_back(group_signal,
							  "item_visible",
							  SceneItemChanged,
							  editor);
			editor->sceneSignals.emplace_back(group_signal,
							  "item_add",
							  ItemRemovedOrAdded,
							  editor);
			editor->sceneSignals.emplace_back(group_signal,
							  "item_remove",
							  ItemRemovedOrAdded,
							  editor);
			return true;
		},
		this);
}

/*
 * Graphics rendering
 */

static void DrawROI(const obs_encoder_roi &roi, const float opacity,
		    gs_eparam_t *colour_param, const uint32_t blockSize)
{
	const uint32_t roi_left = roi.left / blockSize;
	const uint32_t roi_top = roi.top / blockSize;
	const uint32_t roi_right = (roi.right + blockSize - 1) / blockSize;
	const uint32_t roi_bottom = (roi.bottom + blockSize - 1) / blockSize;

	float red = roi.priority < 0.0f ? -roi.priority : 0.0f;
	float green = roi.priority > 0.0f ? roi.priority : 0.0f;
	float blue = std::max(0.5f - std::abs(roi.priority), 0.0f);

	vec4 fillColor;
	vec4_set(&fillColor, red, green, blue, opacity);

	gs_matrix_push();
	gs_matrix_identity();

	gs_matrix_translate3f(roi_left, roi_top, 0.0f);
	gs_matrix_scale3f(roi_right - roi_left, roi_bottom - roi_top, 1.0f);

	gs_effect_set_vec4(colour_param, &fillColor);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
}

void RoiEditor::CreatePreviewTexture(RoiEditor *editor, uint32_t cx,
				     uint32_t cy)
{
	static size_t draw_until_layer = 1;

	if (!editor->texRender)
		editor->texRender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	if (!editor->rectFill) {
		gs_render_start(true);

		gs_vertex2f(0.0f, 0.0f);
		gs_vertex2f(1.0f, 0.0f);
		gs_vertex2f(0.0f, 1.0f);
		gs_vertex2f(1.0f, 1.0f);

		editor->rectFill = gs_render_save();
	}

	const uint32_t block_width =
		(cx + (editor->texBlockSize - 1)) / editor->texBlockSize;
	const uint32_t block_height =
		(cy + (editor->texBlockSize - 1)) / editor->texBlockSize;
	const float opacity = (float)editor->texOpacity / 100.0f;

	gs_texrender_reset(editor->texRender);

	if (gs_texrender_begin(editor->texRender, block_width, block_height)) {
		vec4 clear_color;
		vec4_zero(&clear_color);
		// Darken background with increasing opacity to increase contrast
		clear_color.w = opacity;

		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

		gs_ortho(0.0f, (float)block_width, 0.0f, (float)block_height,
			 -100.0f, 100.0f);

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_technique_t *tech = gs_effect_get_technique(effect, "Solid");
		gs_eparam_t *colour_param =
			gs_effect_get_param_by_name(effect, "color");

		gs_blend_state_push();
		gs_enable_blending(false);

		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);
		gs_load_vertexbuffer(editor->rectFill);

		editor->preview_roi_mutex.lock();
		// Regions have to be drawn back to front
		size_t ctr = 0;
		for (auto it = editor->preview_roi.rbegin();
		     it != editor->preview_roi.rend(); ++it, ++ctr) {
			if (editor->debug_draw && ctr > draw_until_layer)
				break;

			const obs_encoder_roi &roi = *it;
			DrawROI(roi, opacity, colour_param,
				editor->texBlockSize);

			// Immediately clear lmao
			if (editor->debug_draw_single && ctr < draw_until_layer)
				gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		}

		if (editor->debug_draw && ctr < draw_until_layer)
			draw_until_layer = 1;
		else if (editor->debug_draw)
			draw_until_layer++;

		editor->preview_roi_mutex.unlock();

		gs_load_vertexbuffer(nullptr);
		gs_technique_end_pass(tech);
		gs_technique_end(tech);

		gs_blend_state_pop();

		gs_texrender_end(editor->texRender);
	}

	editor->rebuild_texture = editor->debug_draw;
}

void RoiEditor::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	RoiEditor *editor = static_cast<RoiEditor *>(data);

	uint32_t base_width = editor->editCanvasWidth;
	uint32_t base_height = editor->editCanvasHeight;

	if (!base_width || !base_height) {
		obs_video_info ovi;
		if (!obs_get_video_info(&ovi))
			return;
		base_width = ovi.base_width;
		base_height = ovi.base_height;
	}

	int viewport_x, viewport_y;
	float scale;

	GetScaleAndCenterPos(base_width, base_height, cx, cy, viewport_x,
			     viewport_y, scale);

	int viewport_width = int(scale * float(base_width));
	int viewport_height = int(scale * float(base_height));

	/* Rebuild preview texture if necessary */
	if (editor->rebuild_texture)
		CreatePreviewTexture(editor, base_width, base_height);

	if (!editor->pointSampler) {
		gs_sampler_info point_sampler = {};
		point_sampler.max_anisotropy = 1;
		editor->pointSampler = gs_samplerstate_create(&point_sampler);
	}

	gs_viewport_push();
	gs_projection_push();

	gs_ortho(0.0f, float(base_width), 0.0f, float(base_height), -100.0f,
		 100.0f);
	gs_set_viewport(viewport_x, viewport_y, viewport_width,
			viewport_height);

	if (editor->texOpacity < 100) {
		if (editor->previewSource) {
			OBSSourceAutoRelease source =
				obs_weak_source_get_source(
					editor->previewSource);
			obs_source_video_render(source);
		} else {
			obs_render_main_texture_src_color_only();
		}
	}

	/* Draw map texture if we have it */
	if (editor->texRender) {
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *param =
			gs_effect_get_param_by_name(effect, "image");
		gs_texture_t *tex = gs_texrender_get_texture(editor->texRender);

		gs_effect_set_next_sampler(param, editor->pointSampler);
		gs_effect_set_texture_srgb(param, tex);

		const float texScale = (float)editor->texBlockSize;

		gs_matrix_push();
		gs_matrix_scale3f(texScale, texScale, 1.0f);

		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(tex, 0, 0, 0);

		gs_matrix_pop();

		gs_enable_framebuffer_srgb(previous);
	}

	/* Draw exact region rectangles so users can tell the precise target
	 * area apart from the encoder's block-granularity coverage. */
	editor->preview_roi_mutex.lock();
	if (!editor->preview_outlines.empty()) {
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color =
			gs_effect_get_param_by_name(solid, "color");
		gs_effect_set_color(color, 0xFFFFFFFF);

		while (gs_effect_loop(solid, "Solid")) {
			for (const obs_encoder_roi &roi :
			     editor->preview_outlines) {
				gs_render_start(false);
				gs_vertex2f((float)roi.left, (float)roi.top);
				gs_vertex2f((float)roi.right, (float)roi.top);
				gs_vertex2f((float)roi.right,
					    (float)roi.bottom);
				gs_vertex2f((float)roi.left,
					    (float)roi.bottom);
				gs_vertex2f((float)roi.left, (float)roi.top);
				gs_render_stop(GS_LINESTRIP);
			}
		}
	}
	editor->preview_roi_mutex.unlock();

	gs_projection_pop();
	gs_viewport_pop();
}

/*
 * Loading/Saving data from/to scene collection
 */

void RoiEditor::LoadRoisFromOBSData(obs_data_t *obj)
{
	ui->enableRoi->setChecked(obs_data_get_bool(obj, "enabled"));
	ui->excludeRecordings->setChecked(
		obs_data_get_bool(obj, "ignore_recording_encoder"));

	debug_draw = obs_data_get_bool(obj, "debug_draw");
	debug_draw_single = obs_data_get_bool(obj, "debug_draw_single");
	enumerate_all_encoders =
		obs_data_get_bool(obj, "enumerate_all_encoders");

	if (const char *geo = obs_data_get_string(obj, "window_geometry"))
		geometry = QByteArray::fromBase64(geo);

	if (obs_data_has_user_value(obj, "opacity"))
		ui->previewOpacity->setValue(obs_data_get_int(obj, "opacity"));


	if (roi_toggle_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		OBSDataArrayAutoRelease hotkey =
			obs_data_get_array(obj, "toggle_hotkey");
		if (hotkey)
			obs_hotkey_load(roi_toggle_hotkey_id, hotkey);
	}

	OBSDataAutoRelease scenes = obs_data_get_obj(obj, "scenes");
	obs_data_item *item = obs_data_first(scenes);

	roi_data.clear();

	while (item) {
		const char *uuid = obs_data_item_get_name(item);

		OBSDataArrayAutoRelease arr = obs_data_item_get_array(item);
		size_t count = obs_data_array_count(arr);

		for (size_t idx = 0; idx < count; idx++) {
			roi_data[uuid].emplace_back(
				obs_data_array_item(arr, idx));
		}

		obs_data_item_next(&item);
	}
}

void RoiEditor::SaveRoisToOBSData(obs_data_t *obj) const
{
	OBSDataAutoRelease scenes = obs_data_create();

	for (const auto &item : roi_data) {
		obs_data_array_t *scene = obs_data_array_create();

		for (const auto &roi : item.second)
			obs_data_array_push_back(scene, roi);

		obs_data_set_array(scenes, item.first.c_str(), scene);
		obs_data_array_release(scene);
	}

	if (debug_draw) {
		obs_data_set_bool(obj, "debug_draw", debug_draw);
		obs_data_set_bool(obj, "debug_draw_single", debug_draw_single);
	}

	if (roi_toggle_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		OBSDataArrayAutoRelease hotkey =
			obs_hotkey_save(roi_toggle_hotkey_id);
		obs_data_set_array(obj, "toggle_hotkey", hotkey);
	}

	obs_data_set_bool(obj, "enabled", ui->enableRoi->isChecked());
	obs_data_set_obj(obj, "scenes", scenes);
	obs_data_set_int(obj, "opacity", ui->previewOpacity->value());
	obs_data_set_string(obj, "window_geometry",
			    saveGeometry().toBase64().constData());
	obs_data_set_bool(obj, "enumerate_all_encoders",
			  enumerate_all_encoders);
	obs_data_set_bool(obj, "ignore_recording_encoder",
			  ui->excludeRecordings->isChecked());
}

/*
 * RoiData
 */

RoiData RoiData::fromObsData(obs_data_t *obj)
{
	RoiData data{};

	data.scene_item_name = obs_data_get_string(obj, "scene_item_name");
	data.posX = obs_data_get_int(obj, "x");
	data.posY = obs_data_get_int(obj, "y");
	data.width = obs_data_get_int(obj, "width");
	data.height = obs_data_get_int(obj, "height");
	data.inner_radius = obs_data_get_int(obj, "center_radius_inner");
	data.inner_steps = obs_data_get_int(obj, "center_steps_inner");
	data.inner_aspect = obs_data_get_bool(obj, "center_aspect_inner");
	data.inner_circle = obs_data_get_bool(obj, "center_circle");
	data.outer_radius = obs_data_get_int(obj, "center_radius_outer");
	data.outer_steps = obs_data_get_int(obj, "center_steps_outer");
	data.outer_priority =
		(float)obs_data_get_double(obj, "center_priority_outer");
	data.outer_aspect = obs_data_get_bool(obj, "center_aspect_outer");
	data.smoothing_steps = obs_data_get_int(obj, "smoothing_steps");
	data.smoothing_type = obs_data_get_int(obj, "smoothing_type");
	data.smoothing_priority =
		obs_data_get_double(obj, "smoothing_priority");
	data.enabled = obs_data_get_bool(obj, "enabled");
	data.priority = (float)obs_data_get_double(obj, "priority");
	data.per_codec_priority =
		obs_data_get_bool(obj, "per_codec_priority");
	data.priority_h264 = (float)obs_data_get_double(obj, "priority_h264");
	data.priority_hevc = (float)obs_data_get_double(obj, "priority_hevc");
	data.priority_av1 = (float)obs_data_get_double(obj, "priority_av1");

	data.padding = obs_data_get_int(obj, "padding");

	// Leave these as default values unless specified otherwise
	if (obs_data_has_user_value(obj, "scene_item_id"))
		data.scene_item_id = obs_data_get_int(obj, "scene_item_id");
	if (obs_data_has_user_value(obj, "scene_item_group_id"))
		data.scene_item_group_id =
			obs_data_get_int(obj, "scene_item_group_id");
	if (obs_data_has_user_value(obj, "center_x"))
		data.center_x = obs_data_get_int(obj, "center_x");
	if (obs_data_has_user_value(obj, "center_y"))
		data.center_y = obs_data_get_int(obj, "center_y");

	return data;
}

/*
 * RoiListItem
 */

QVariant RoiListItem::data(int role) const
{
	if (role == ROIData)
		return QVariant::fromValue<RoiData>(roi);

	return QListWidgetItem::data(role);
}

void RoiListItem::setData(int role, const QVariant &value)
{
	if (role != ROIData) {
		QListWidgetItem::setData(role, value);
		return;
	}

	roi = value.value<RoiData>();

	QString desc;

	// This needs to be prettier at some point
	if (!roi.enabled) {
		desc += "[";
		desc += obs_module_text("ROI.Item.DisabledPrefix");
		desc += "] ";
	}

	if (type() == Manual) {
		desc += QString(obs_module_text("ROI.Item.ManualRegion"))
				.arg(roi.width)
				.arg(roi.height)
				.arg(roi.posX)
				.arg(roi.posY);
	} else if (type() == SceneItem) {
		desc += QString(obs_module_text("ROI.Item.SceneItem"))
				.arg(roi.scene_item_name)
				.arg(roi.scene_item_id);
	} else {
		desc += obs_module_text("ROI.Item.CenterFocus");
	}

	setText(desc);
}

/*
 * Frontend Event Handlers
 */

static void SaveRoiEditor(obs_data_t *save_data, bool saving, void *)
{
	if (saving) {
		OBSDataAutoRelease obj = obs_data_create();
		roi_edit->SaveRoisToOBSData(obj);
		obs_data_set_obj(save_data, "roi", obj);
	} else {
		OBSDataAutoRelease obj = obs_data_get_obj(save_data, "roi");
		if (obj)
			roi_edit->LoadRoisFromOBSData(obj);
	}
}

static void OBSEvent(obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		roi_edit->UpdateEncoders();
		roi_edit->ConnectSceneSignals();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		roi_edit->UpdateEncoders();
		break;
	/* SCRIPTING_SHUTDOWN is the last event before OBSBasic::ClearSceneData
	 * destroys scenes and canvases; EXIT only fires afterwards, which is
	 * too late to disconnect signal handlers safely. */
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		roi_edit->PrepareShutdown();
		break;
	default:
		break;
	}
}

static void RoiToggleHotkey(void *, obs_hotkey_id, obs_hotkey_t *,
			    bool pressed)
{
	if (pressed && roi_edit)
		QMetaObject::invokeMethod(roi_edit, "ToggleRoiEnabled",
					  Qt::QueuedConnection);
}

/*
 * C stuff
 */

extern "C" void InitRoiEditor()
{
	auto action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(
			obs_module_text("ROIEditor")));
	auto window =
		static_cast<QMainWindow *>(obs_frontend_get_main_window());

	/* Push translation function so that strings in .ui file are translated */
	obs_frontend_push_ui_translation(obs_module_get_string);
	roi_edit = new RoiEditor(window);
	obs_frontend_pop_ui_translation();

	roi_toggle_hotkey_id = obs_hotkey_register_frontend(
		"obs-roi-ui.toggle", obs_module_text("ROI.Hotkey.Toggle"),
		RoiToggleHotkey, nullptr);

	obs_frontend_add_save_callback(SaveRoiEditor, nullptr);
	obs_frontend_add_event_callback(OBSEvent, nullptr);

	QAction::connect(action, &QAction::triggered, roi_edit,
			 &RoiEditor::ShowHideDialog);
}
