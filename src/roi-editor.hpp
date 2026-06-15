#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <string>

#include "ui_roi-editor.h"

#include <obs.hpp>

class RoiListItem;
class QCloseEvent;

// ToDo add type to data as well?
struct RoiData {
	/* Scene item type*/
	QString scene_item_name;
	int64_t scene_item_id = -1;
	/* Parent group item id if the scene item is inside a group */
	int64_t scene_item_group_id = -1;
	int64_t padding = 0;
	/* Manual type */
	uint32_t posX, posY, width, height;
	/* Center focus type */
	int64_t inner_radius;
	int64_t inner_steps;
	bool inner_aspect;
	bool inner_circle;
	int64_t outer_radius;
	int64_t outer_steps;
	float outer_priority;
	bool outer_aspect;
	int32_t center_x = -1;
	int32_t center_y = -1;
	/* Shared attributes */
	int smoothing_type;
	int smoothing_steps;
	float smoothing_priority;
	bool enabled;
	float priority;
	/* Optional per-codec priority overrides */
	bool per_codec_priority = false;
	float priority_h264 = 0.0f;
	float priority_hevc = 0.0f;
	float priority_av1 = 0.0f;

	static RoiData fromObsData(obs_data_t *obj);
};
Q_DECLARE_METATYPE(RoiData)

class RoiEditor : public QDialog {
	Q_OBJECT

	enum Direction { Up, Down };

public:
	enum Smoothing { None, Inside, Outside, Edge };

	std::unique_ptr<Ui_ROIEditor> ui;
	RoiEditor(QWidget *parent);
	~RoiEditor()
	{
		obs_enter_graphics();
		gs_texrender_destroy(texRender);
		gs_samplerstate_destroy(pointSampler);
		gs_vertexbuffer_destroy(rectFill);
		obs_leave_graphics();
	}

	void ConnectSceneSignals();
	void ConnectSignalsForScene(obs_source_t *source);
	void PrepareShutdown();
	void LoadRoisFromOBSData(obs_data_t *obj);
	void SaveRoisToOBSData(obs_data_t *obj) const;

public slots:
	void ShowHideDialog();
	void UpdateEncoders();
	void RefreshSceneList();
	void ToggleRoiEnabled();

public:
	/* Used by the encoder preview to drive the ROI master switch for
	 * realtime A/B testing. */
	void SetRoiFeatureEnabled(bool enabled);
	bool RoiFeatureEnabled() const;

	/* Per-encoder "tight fit": round regions to the nearest block (hugs
	 * the source, may trim edges) instead of expanding outward to fully
	 * cover it. Keyed by encoder name. */
	void SetEncoderTightFit(const std::string &name, bool tight);
	bool EncoderTightFit(const std::string &name) const;

private slots:
	void on_actionAddRoi_triggered();
	void on_actionRemoveRoi_triggered();
	void on_actionRoiUp_triggered();
	void on_actionRoiDown_triggered();
	void on_actionCopyRegions_triggered();
	void on_actionImportRegions_triggered();
	void on_actionExportRegions_triggered();
	void on_actionVerifyRoi_triggered();

	void SceneSelectionChanged();
	void ItemSelected(QListWidgetItem *item, QListWidgetItem *);
	void PropertiesChanges();
	void UpdatePreview();

	void RefreshData();
	void RefreshSceneItems();

private:
	void AddRegionItem(int type);
	void AddBackgroundItem();

	void RegionItemsToData();
	void RegionItemsFromData();

	std::vector<obs_encoder_roi> RegionsFromData(const std::string &uuid,
						     const char *codec = nullptr);
	void MoveRoiItem(Direction direction);
	void CreateDisplay(bool recreate = false);
	void SetStatusLabel(const QStringList &encoder_names);
	void UpdateEditCanvasSize();
	bool PreviewToCanvas(const QPointF &pos, uint32_t &canvas_x,
			     uint32_t &canvas_y);

	void closeEvent(QCloseEvent *event) override;
	bool eventFilter(QObject *obj, QEvent *event) override;

	static void SceneItemChanged(void *param, calldata_t *data);
	static void ItemRemovedOrAdded(void *param, calldata_t *data);
	static void CanvasChannelChanged(void *param, calldata_t *data);
	static void DrawPreview(void *data, uint32_t cx, uint32_t cy);
	static void CreatePreviewTexture(RoiEditor *editor, uint32_t cx,
					 uint32_t cy);

	// All signals are added/cleared at once, so just store them in a vector somewhere
	std::vector<OBSSignal> sceneSignals;

	// key is scene UUID
	std::unordered_map<std::string, std::vector<OBSDataAutoRelease>>
		roi_data;

	bool enumerate_all_encoders = false;

	/* Encoder names that should use tight-fit alignment */
	std::set<std::string> tightFitEncoders;

	/* Set on OBS_FRONTEND_EVENT_EXIT: signal handlers are disconnected
	 * while still valid, and late callbacks must become no-ops. */
	std::atomic_bool shuttingDown = false;

	std::vector<obs_encoder_roi>
	RegionOutlinesFromData(const std::string &uuid);

	// Rendering stuff
	std::mutex preview_roi_mutex;
	std::vector<obs_encoder_roi> preview_roi;
	/* Exact (unsnapped) region rectangles for the preview outlines */
	std::vector<obs_encoder_roi> preview_outlines;
	OBSWeakSourceAutoRelease previewSource;

	bool debug_draw = false;
	bool debug_draw_single = false;
	bool rebuild_texture = false;
	uint32_t texOpacity;
	uint32_t texBlockSize;

	/* Base dimensions of the canvas owning the scene being edited,
	 * written on the UI thread and read by the graphics thread. */
	std::atomic<uint32_t> editCanvasWidth = 0;
	std::atomic<uint32_t> editCanvasHeight = 0;

	gs_texrender_t *texRender = nullptr;
	gs_samplerstate_t *pointSampler = nullptr;
	gs_vertbuffer_t *rectFill = nullptr;

	// Qt stuff
	RoiListItem *currentItem = nullptr;
	QByteArray geometry;

	// Drag-to-draw state (preview mouse interaction)
	RoiListItem *dragItem = nullptr;
	uint32_t dragStartX = 0;
	uint32_t dragStartY = 0;
};

extern RoiEditor *roi_edit;

/* GPU index an NVENC encoder runs on (-1 = default/compositing GPU,
 * -2 = not an NVENC encoder). */
int RoiEncoderGpuIndex(obs_encoder_t *enc);

enum ROIDataRoles { ROIData = Qt::UserRole };

class RoiListItem : public QListWidgetItem {

public:
	enum RoiItemType {
		SceneItem = QListWidgetItem::UserType,
		Manual,
		CenterFocus,
	};

	RoiListItem(int type) : QListWidgetItem(nullptr, type) {}

	QVariant data(int role) const override;
	void setData(int role, const QVariant &value) override;

private:
	RoiData roi;
};
