#include "voxel_blocky_texture_atlas_editor.h"
#include "../../constants/voxel_string_names.h"
#include "../../meshers/blocky/blocky_connected_textures.h"
#include "../../util/containers/container_funcs.h"
#include "../../util/godot/classes/button.h"
#include "../../util/godot/classes/confirmation_dialog.h"
#include "../../util/godot/classes/editor_file_system.h"
#include "../../util/godot/classes/editor_interface.h"
#include "../../util/godot/classes/h_box_container.h"
#include "../../util/godot/classes/h_split_container.h"
#include "../../util/godot/classes/image.h"
#include "../../util/godot/classes/input_event_mouse_button.h"
#include "../../util/godot/classes/input_event_mouse_motion.h"
#include "../../util/godot/classes/item_list.h"
#include "../../util/godot/classes/line_edit.h"
#include "../../util/godot/classes/popup_menu.h"
#include "../../util/godot/classes/spin_box.h"
#include "../../util/godot/classes/v_box_container.h"
#include "../../util/godot/classes/v_separator.h"
#include "../../util/godot/core/array.h"
#include "../../util/godot/core/mouse_button.h"
#include "../../util/godot/core/packed_arrays.h"
#include "../../util/godot/core/string.h"
#include "../../util/godot/editor_scale.h"
#include "../../util/godot/inspector/inspector.h"
#include "../../util/godot/pan_zoom_container.h"
#include "../../util/math/vector2i.h"

namespace zylann::voxel {

// TODO Figure out a generic approach to blob tilesets.
// Instead of expecting our own 12x4 layout (as seen from OptiFine CTM), allow the user to multi-select tiles, create a
// Blob9 tile, then allow to define the layout by drawing a pattern of connections over the existing layout, so we can
// then recognize which tiles are available and validate it?
// This sounds feasible but it's a bunch of work, for now we'll support only the fixed layout.

namespace {
enum ContextMenuID {
	MENU_CREATE_SINGLE_TILE,
	MENU_CREATE_GRID_OF_TILES,
	MENU_CREATE_BLOB9_TILE,
	MENU_CREATE_RANDOM_TILE,
	MENU_CREATE_EXTENDED_TILE,
	MENU_REMOVE_TILE,
	MENU_RENAME_TILE,
	MENU_GENERATE_FROM_COMPACT5
};
}

VoxelBlockyTextureAtlasEditor::VoxelBlockyTextureAtlasEditor() {
	const float editor_scale = EDSCALE;

	HSplitContainer *split_container = memnew(HSplitContainer);
	split_container->set_split_offset(editor_scale * 200.f);
	split_container->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);

	const VoxelStringNames &sn = VoxelStringNames::get_singleton();

	{
		VBoxContainer *vb = memnew(VBoxContainer);
		_tile_list = memnew(ItemList);
		_tile_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
		_tile_list->connect(
				sn.item_selected, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_tile_list_item_selected)
		);
		_tile_list->connect(
				sn.item_clicked, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_tile_list_item_clicked)
		);
		vb->add_child(_tile_list);
		split_container->add_child(vb);
	}
	{
		HSplitContainer *split_container2 = memnew(HSplitContainer);

		{
			VBoxContainer *mid_container = memnew(VBoxContainer);
			// So the parent split container will resize this control upon resizing, instead of the one on the right
			mid_container->set_h_size_flags(Control::SIZE_EXPAND_FILL);

			{
				_toolbar_container = memnew(HBoxContainer);

				Ref<ButtonGroup> mode_button_group;
				mode_button_group.instantiate();

				{
					Button *button = memnew(Button);
					button->set_toggle_mode(true);
					button->set_tooltip_text("Select Existing Tiles");
					button->set_button_group(mode_button_group);
					button->set_theme_type_variation(sn.FlatButton);
					button->set_meta(sn.mode, MODE_SELECT);
					_toolbar_container->add_child(button);
					_mode_buttons[MODE_SELECT] = button;
				}
				{
					Button *button = memnew(Button);
					button->set_toggle_mode(true);
					button->set_tooltip_text("Create New Tiles");
					button->set_button_group(mode_button_group);
					button->set_theme_type_variation(sn.FlatButton);
					button->set_meta(sn.mode, MODE_CREATE);
					_toolbar_container->add_child(button);
					_mode_buttons[MODE_CREATE] = button;
				}

				_mode_buttons[_mode]->set_pressed(true);

				mode_button_group->connect(
						sn.pressed, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_mode_button_group_pressed)
				);

				_toolbar_container->add_child(memnew(VSeparator));

				// Might become a mode in the future, to edit connectivity
				_connectivity_button = memnew(Button);
				_connectivity_button->set_toggle_mode(true);
				_connectivity_button->set_tooltip_text("Show Connectivity");
				_connectivity_button->set_theme_type_variation(sn.FlatButton);
				_connectivity_button->connect(
						sn.toggled, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_connectivity_button_toggled)
				);
				_toolbar_container->add_child(_connectivity_button);

				mid_container->add_child(_toolbar_container);
			}
			{
				_pan_zoom_container = memnew(ZN_PanZoomContainer);
				_pan_zoom_container->set_v_size_flags(Control::SIZE_EXPAND_FILL);

				_texture_rect = memnew(Control);
				_texture_rect->set_mouse_filter(MOUSE_FILTER_PASS);
				_texture_rect->set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST);
				_texture_rect->connect(
						sn.draw, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_texture_rect_draw)
				);
				_texture_rect->connect(
						sn.gui_input, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_texture_rect_gui_input)
				);
				// _texture_rect->set_stretch_mode(TextureRect::STRETCH_KEEP);
				_texture_rect->set_focus_mode(FOCUS_ALL);
				_pan_zoom_container->add_child(_texture_rect);

				mid_container->add_child(_pan_zoom_container);
			}
			{
				_blob9_gen.container = memnew(HBoxContainer);

				{
					Label *label = memnew(Label);
					label->set_text("Margins: ");
					_blob9_gen.container->add_child(label);
				}

				_blob9_gen.margin_x_spinbox = memnew(SpinBox);
				_blob9_gen.margin_x_spinbox->set_step(1.f);
				_blob9_gen.margin_x_spinbox->connect(
						"value_changed",
						callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_blob9_gen_margin_spinbox_value_changed)
				);
				_blob9_gen.container->add_child(_blob9_gen.margin_x_spinbox);

				_blob9_gen.margin_y_spinbox = memnew(SpinBox);
				_blob9_gen.margin_y_spinbox->set_step(1.f);
				_blob9_gen.margin_y_spinbox->connect(
						"value_changed",
						callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_blob9_gen_margin_spinbox_value_changed)
				);
				_blob9_gen.container->add_child(_blob9_gen.margin_y_spinbox);

				{
					Control *spacer = memnew(Control);
					spacer->set_h_size_flags(Control::SIZE_EXPAND_FILL);
					_blob9_gen.container->add_child(spacer);
				}

				{
					Button *button = memnew(Button);
					button->set_text(ZN_TTR("Apply (no undo)"));
					button->connect(
							sn.pressed,
							callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_blob9_gen_apply_button_pressed)
					);
					_blob9_gen.container->add_child(button);
				}
				{
					Button *button = memnew(Button);
					button->set_text(ZN_TTR("Cancel"));
					button->connect(
							sn.pressed,
							callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_blob9_gen_cancel_button_pressed)
					);
					_blob9_gen.container->add_child(button);
				}

				_blob9_gen.container->hide();
				mid_container->add_child(_blob9_gen.container);
			}

			split_container2->add_child(mid_container);
		}
		{
			_inspector = memnew(ZN_Inspector);
			_inspector->set_custom_minimum_size(Vector2(100, 0));
			_inspector->set_v_size_flags(Control::SIZE_EXPAND_FILL);
			split_container2->add_child(_inspector);
		}

		split_container->add_child(split_container2);
	}

	{
		_select_context_menu = memnew(PopupMenu);
		_select_context_menu->connect(
				sn.id_pressed, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_context_menu_id_pressed)
		);
		add_child(_select_context_menu);
	}
	{
		_create_context_menu = memnew(PopupMenu);
		_create_context_menu->add_item("Create Single Tile", MENU_CREATE_SINGLE_TILE);
		_create_context_menu->add_item("Create Grid of Tiles", MENU_CREATE_GRID_OF_TILES);
		_create_context_menu->add_item("Create Blob9 Tile", MENU_CREATE_BLOB9_TILE);
		_create_context_menu->add_item("Create Random Tile", MENU_CREATE_RANDOM_TILE);
		_create_context_menu->add_item("Create Extended Tile", MENU_CREATE_EXTENDED_TILE);
		_create_context_menu->connect(
				sn.id_pressed, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_context_menu_id_pressed)
		);
		add_child(_create_context_menu);
	}

	add_child(split_container);

	// set_process(true);
}

void VoxelBlockyTextureAtlasEditor::update_select_context_menu() {
	_select_context_menu->clear();

	_select_context_menu->add_item("Rename Tile", MENU_RENAME_TILE);

	const int tid = get_selected_tile_id();
	if (tid != -1) {
		if (_atlas.is_valid()) {
			const VoxelBlockyTextureAtlas::TileType tt = _atlas->get_tile_type(tid);
			if (tt == VoxelBlockyTextureAtlas::TILE_TYPE_BLOB9) {
				const int idx = _select_context_menu->get_item_count();
				_select_context_menu->add_item("Generate tiles...", MENU_GENERATE_FROM_COMPACT5);
				_select_context_menu->set_item_tooltip(
						idx,
						ZN_TTR("Generates Blob9 tiles from 5 reference tiles (point, horizontal, vertical, cross, "
							   "full).\nThis operation will modify the image file. Undo is not implemented.")
				);
			}
		}
	}

	_select_context_menu->add_separator();
	_select_context_menu->add_item("Remove Tile", MENU_REMOVE_TILE);
}

void VoxelBlockyTextureAtlasEditor::make_read_only() {
	_inspector->hide();
	_toolbar_container->hide();
	_read_only = true;
}

void VoxelBlockyTextureAtlasEditor::set_godot_editor_interface(EditorInterface *editor_interface) {
	ZN_ASSERT_RETURN(editor_interface != nullptr);
	ZN_ASSERT_RETURN(_godot_editor_interface == nullptr);

	_godot_editor_interface = editor_interface;

	EditorFileSystem *efs = _godot_editor_interface->get_resource_file_system();
	efs->connect("resources_reimported", callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_resources_reimported));
}

// I'd like rename by clicking on the item like with scene tree nodes, but ItemList doesn't provide that functionality,
// and it's a bit tricky to do. Not impossible, but for now let's use a safe approach
void VoxelBlockyTextureAtlasEditor::open_rename_dialog() {
	ZN_ASSERT_RETURN(_atlas.is_valid());

	const int tile_id = get_selected_tile_id();
	ZN_ASSERT_RETURN(tile_id >= 0);

	if (_rename_popup == nullptr) {
		_rename_popup = memnew(ConfirmationDialog);
		_rename_popup->connect(
				VoxelStringNames::get_singleton().confirmed,
				callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_rename_popup_confirmed)
		);

		_rename_line_edit = memnew(LineEdit);
		_rename_popup->add_child(_rename_line_edit);

		add_child(_rename_popup);
	}

	const String old_name = _atlas->get_tile_name(tile_id);

	_rename_line_edit->set_text(old_name);
	_rename_line_edit->select_all();

	_rename_popup->set_title(String("Rename tile \"{0}\"").format(varray(old_name)));
	_rename_popup->popup_centered();

	_rename_line_edit->grab_focus();
}

void VoxelBlockyTextureAtlasEditor::set_atlas(Ref<VoxelBlockyTextureAtlas> atlas) {
	if (atlas == _atlas) {
		return;
	}

	const VoxelStringNames &sn = VoxelStringNames::get_singleton();

	if (_atlas.is_valid()) {
		_atlas->disconnect(sn.changed, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_atlas_changed));
	}

	_atlas = atlas;

	if (_atlas.is_valid()) {
		_atlas->connect(sn.changed, callable_mp(this, &VoxelBlockyTextureAtlasEditor::on_atlas_changed));

		const Vector2i size(_atlas->get_resolution());
		_pan_zoom_container->set_content_rect(Rect2(Vector2(), Vector2(size)));
	}

	update_tile_list();

	if (_atlas.is_valid()) {
		update_texture_rect();
		set_selected_tile_id(-1, true);
		set_tile_selection_rect(Rect2i());
	}
}

int VoxelBlockyTextureAtlasEditor::get_selected_tile_id() const {
	const PackedInt32Array selection = _tile_list->get_selected_items();
	if (selection.size() == 0) {
		return -1;
	}
	const int i = selection[0];
	const int tile_id = _tile_list->get_item_metadata(i);
	return tile_id;
}

static void draw_grid(
		CanvasItem &ci,
		const Vector2 origin,
		const Vector2i grid_size,
		const Vector2 spacing,
		const Color color
) {
	for (int y = 0; y < grid_size.y + 1; ++y) {
		ci.draw_line(
				origin + Vector2(0, y * spacing.y), //
				origin + Vector2(grid_size.x * spacing.x, y * spacing.y),
				color
		);
	}
	for (int x = 0; x < grid_size.x + 1; ++x) {
		ci.draw_line(
				origin + Vector2(x * spacing.x, 0), //
				origin + Vector2(x * spacing.x, grid_size.y * spacing.y),
				color
		);
	}
}

void VoxelBlockyTextureAtlasEditor::_notification(int p_what) {
	switch (p_what) {
			// case NOTIFICATION_ENTER_TREE: {
			// } break;

		case NOTIFICATION_THEME_CHANGED: {
			const VoxelStringNames &sn = VoxelStringNames::get_singleton();
			{
				Ref<Texture2D> icon = get_theme_icon(sn.ToolSelect, sn.EditorIcons);
				_mode_buttons[MODE_SELECT]->set_button_icon(icon);
			}
			{
				Ref<Texture2D> icon = get_theme_icon(sn.RegionEdit, sn.EditorIcons);
				_mode_buttons[MODE_CREATE]->set_button_icon(icon);
			}
			{
				Ref<Texture2D> icon = get_theme_icon(sn.TerrainMatchCornersAndSides, sn.EditorIcons);
				_connectivity_button->set_button_icon(icon);
			}
		} break;

			// case NOTIFICATION_PROCESS: {
			// } break;

		default:
			break;
	}
}

static void draw_blob9_connection_mask(
		CanvasItem &ci,
		const Vector2i origin,
		const Vector2i ts,
		const Vector2i margin,
		const uint16_t mask,
		const Color color0,
		const Color color1
) {
	unsigned int i = 0;
	for (unsigned int sy = 0; sy < 3; ++sy) {
		for (unsigned int sx = 0; sx < 3; ++sx) {
			if (sx == 1 && sy == 1) {
				continue;
			}
			// clang-format off
			const Vector2i pos(
					sx == 0 ? 0 : sx == 1 ? margin.x : ts.x - margin.x,
					sy == 0 ? 0 : sy == 1 ? margin.y : ts.y - margin.y
			);
			const Vector2i size(
					sx == 1 ? ts.x - 2 * margin.x : margin.x, 
					sy == 1 ? ts.y - 2 * margin.y : margin.y
			);
			// clang-format on
			const Rect2 rect(origin + pos, size);
			const Color color = (mask & (1 << i)) != 0 ? color1 : color0;
			ci.draw_rect(rect, color, true);
			i += 1;
		}
	}

	ci.draw_rect(Rect2(origin + margin, ts - 2 * margin), color1, true);
}

Vector2i VoxelBlockyTextureAtlasEditor::get_tile_blob9_margin(const int tile_id) const {
	ZN_ASSERT_RETURN_V(_atlas.is_valid(), Vector2i());
	if (_blob9_gen.container->is_visible()) {
		const int selected_tile_id = get_selected_tile_id();
		if (tile_id == selected_tile_id) {
			return Vector2i(_blob9_gen.margin_x_spinbox->get_value(), _blob9_gen.margin_y_spinbox->get_value());
		}
	}
	const Vector2i saved_margin = _atlas->editor_get_tile_blob9_margin(tile_id);
	if (saved_margin != Vector2i()) {
		return saved_margin;
	}
	const Vector2i ts = _atlas->get_default_tile_resolution();
	return ts / 3;
}

void VoxelBlockyTextureAtlasEditor::on_texture_rect_draw() {
	if (!_atlas.is_valid()) {
		return;
	}
	CanvasItem &ci = *_texture_rect;

	if (_blob9_gen.container->is_visible()) {
		ci.draw_texture(_blob9_gen.texture, Vector2i());
	} else {
		ci.draw_texture(_atlas->get_texture(), Vector2());
	}

	const Vector2i ts = math::max(_atlas->get_default_tile_resolution(), Vector2i(1, 1));
	const Vector2i res = _atlas->get_resolution();
	const Vector2i num_tiles = res / ts;

	const int selected_tile_id = get_selected_tile_id();

	const Color grid_color(0, 0, 0, 0.15);
	const Color group_color(1, 1, 1, 0.25);
	const Color hover_color(0.5, 0.5, 1.0, 0.5);
	const Color selection_color(0.5, 0.5, 1.0, 0.5);
	const Color connection_mask_0_color(0.0, 0.0, 0.0, 0.2);
	const Color connection_mask_1_color(1.0, 1.0, 1.0, 0.2);
	const Color connection_mask_compact5_overlay_color(1.0, 1.0, 0.0, 0.1);
	const Color connection_mask_compact5_border_color(1.0, 1.0, 0.0, 0.8);
	const Color connection_mask_compact5_margin_color(1.0, 1.0, 0.0, 0.5);

	draw_grid(ci, Vector2(), num_tiles, Vector2(ts), grid_color);

	const Span<const VoxelBlockyTextureAtlas::Tile> tiles = _atlas->get_tiles();

	if (_connectivity_button->is_pressed()) {
		for (unsigned int tile_id = 0; tile_id < tiles.size(); ++tile_id) {
			const VoxelBlockyTextureAtlas::Tile &tile = tiles[tile_id];
			if (tile.is_tombstone()) {
				continue;
			}
			if (tile.type == VoxelBlockyTextureAtlas::TILE_TYPE_BLOB9) {
				const Vector2i layout_origin(tile.position_x, tile.position_y);

				// This assumes the default layout!

				const std::array<uint8_t, blocky::COMPACT5_TILE_COUNT> compact5_ref_cases =
						blocky::get_blob9_reference_cases_for_compact5();

				const Vector2i margin = get_tile_blob9_margin(tile_id);

				for (unsigned int gy = 0; gy < tile.group_size_y; ++gy) {
					for (unsigned int gx = 0; gx < tile.group_size_x; ++gx) {
						const uint8_t case_index = gx + gy * blocky::BLOB9_DEFAULT_LAYOUT_SIZE_X;
						if (case_index >= blocky::BLOB9_TILE_COUNT) {
							// The last tile is not used
							continue;
						}
						const unsigned int mask = blocky::get_connection_mask_from_case_index(case_index);
						const Vector2i pos = layout_origin + Vector2i(gx, gy) * ts;
						draw_blob9_connection_mask(
								ci, Vector2(pos), ts, margin, mask, connection_mask_0_color, connection_mask_1_color
						);

						if (contains(compact5_ref_cases, case_index)) {
							ci.draw_rect(Rect2(pos, ts), connection_mask_compact5_overlay_color, true);
						}
					}
				}
			}
		}
	}

	for (unsigned int tile_id = 0; tile_id < tiles.size(); ++tile_id) {
		const VoxelBlockyTextureAtlas::Tile &tile = tiles[tile_id];
		if (tile.is_tombstone()) {
			continue;
		}
		ci.draw_rect(
				Rect2(Vector2(tile.position_x, tile.position_y),
					  Vector2(tile.group_size_x, tile.group_size_y) * Vector2(ts)),
				group_color,
				false
		);

		if (_connectivity_button->is_pressed() || _blob9_gen.container->is_visible()) {
			if (tile.type == VoxelBlockyTextureAtlas::TILE_TYPE_BLOB9) {
				const Vector2i layout_origin(tile.position_x, tile.position_y);

				const std::array<uint8_t, blocky::COMPACT5_TILE_COUNT> compact5_ref_cases =
						blocky::get_blob9_reference_cases_for_compact5();

				const Vector2i margin = get_tile_blob9_margin(tile_id);

				for (const uint8_t case_index : compact5_ref_cases) {
					const int tx = case_index % blocky::BLOB9_DEFAULT_LAYOUT_SIZE_X;
					const int ty = case_index / blocky::BLOB9_DEFAULT_LAYOUT_SIZE_X;
					const Vector2i pos = layout_origin + Vector2i(tx, ty) * ts;
					ci.draw_rect(Rect2(pos, ts), connection_mask_compact5_border_color, false);

					PackedVector2Array points;
					points.resize(8);
					Span<Vector2> points_s = to_span(points);

					points_s[0] = pos + Vector2i(margin.x, 0);
					points_s[1] = pos + Vector2i(margin.x, ts.y);
					points_s[2] = pos + Vector2i(ts.x - margin.x, 0);
					points_s[3] = pos + Vector2i(ts.x - margin.x, ts.y);
					points_s[4] = pos + Vector2i(0, margin.y);
					points_s[5] = pos + Vector2i(ts.x, margin.y);
					points_s[6] = pos + Vector2i(0, ts.y - margin.y);
					points_s[7] = pos + Vector2i(ts.x, ts.y - margin.y);

					ci.draw_multiline(points, connection_mask_compact5_margin_color);
				}
			}
		}
	}

	switch (_mode) {
		case MODE_SELECT: {
			if (selected_tile_id != -1) {
				const VoxelBlockyTextureAtlas::Tile &tile = _atlas->get_tile(selected_tile_id);
				ci.draw_rect(
						Rect2(Vector2(tile.position_x, tile.position_y),
							  Vector2(tile.group_size_x, tile.group_size_y) * Vector2(ts)),
						selection_color,
						false,
						2.f
				);
			}

			if (_hovered_tile_id != -1) {
				const VoxelBlockyTextureAtlas::Tile &tile = _atlas->get_tile(_hovered_tile_id);
				ci.draw_rect(
						Rect2(Vector2(tile.position_x, tile.position_y),
							  Vector2(tile.group_size_x, tile.group_size_y) * Vector2(ts)),
						hover_color,
						false
				);
			}
		} break;

		case MODE_CREATE: {
			if (Rect2i(Vector2i(), num_tiles).has_point(_hovered_tile_position)) {
				ci.draw_rect(Rect2(Vector2(_hovered_tile_position * ts), Vector2(ts)), hover_color, false);
			}

			if (_tile_selection_rect != Rect2i()) {
				ci.draw_rect(
						Rect2(_tile_selection_rect.position * ts, _tile_selection_rect.size * ts),
						selection_color,
						false,
						2.f
				);
			}
		} break;
	}
}

void VoxelBlockyTextureAtlasEditor::on_atlas_changed() {
	// TODO Not sure we should handle null atlas here
	if (_atlas.is_null()) {
		update_texture_rect();
		_inspector->clear();
		update_tile_list();
		return;
	}

	switch (_atlas->get_last_change()) {
		case VoxelBlockyTextureAtlas::CHANGE_TEXTURE: {
			const Vector2i size(_atlas->get_resolution());
			_pan_zoom_container->set_content_rect(Rect2(Vector2(), Vector2(size)));

			update_texture_rect();

			if (_blob9_gen.container->is_visible()) {
				update_blob9_gen();
			}
		} break;

		case VoxelBlockyTextureAtlas::CHANGE_TILE_ADDED:
		case VoxelBlockyTextureAtlas::CHANGE_TILE_REMOVED:
			update_tile_list();
			_texture_rect->queue_redraw();
			break;

		case VoxelBlockyTextureAtlas::CHANGE_TILE_MODIFIED:
			_texture_rect->queue_redraw();
			break;

		default:
			ZN_PRINT_ERROR("Unhandled change");
			break;
	}
}

inline bool is_tile_rect_valid(const Rect2i rect) {
	return rect != Rect2i() && rect.position.x >= 0 && rect.position.y >= 0 && rect.size.x >= 1 && rect.size.y >= 1;
}

void VoxelBlockyTextureAtlasEditor::on_context_menu_id_pressed(int id) {
	ERR_FAIL_COND(_atlas.is_null());
	const Vector2i ts = _atlas->get_default_tile_resolution();

	switch (id) {
		case MENU_CREATE_SINGLE_TILE: {
			const Rect2i rect = _tile_selection_rect;
			ZN_ASSERT_RETURN(is_tile_rect_valid(rect));

			VoxelBlockyTextureAtlas::Tile tile;
			tile.type = blocky::TILE_SINGLE;
			tile.position_x = rect.position.x * ts.x;
			tile.position_y = rect.position.y * ts.y;
			tile.group_size_x = 1;
			tile.group_size_y = 1;

			// TODO UndoRedo
			_atlas->add_tile(tile);
			on_atlas_changed();
		} break;

		case MENU_CREATE_GRID_OF_TILES: {
			ZN_ASSERT_RETURN(is_tile_rect_valid(_tile_selection_rect));

			Vector2i tpos;
			const Vector2i origin = _tile_selection_rect.position * ts;
			for (tpos.y = 0; tpos.y < _tile_selection_rect.size.y; ++tpos.y) {
				for (tpos.x = 0; tpos.x < _tile_selection_rect.size.x; ++tpos.x) {
					VoxelBlockyTextureAtlas::Tile tile;
					tile.type = blocky::TILE_SINGLE;
					tile.position_x = origin.x + tpos.x * ts.x;
					tile.position_y = origin.y + tpos.y * ts.y;
					tile.group_size_x = 1;
					tile.group_size_y = 1;

					// TODO UndoRedo
					_atlas->add_tile(tile);
				}
			}

			on_atlas_changed();
		} break;

		case MENU_CREATE_BLOB9_TILE: {
			ZN_ASSERT_RETURN(is_tile_rect_valid(_tile_selection_rect));

			VoxelBlockyTextureAtlas::Tile tile;
			tile.type = blocky::TILE_BLOB9;
			tile.position_x = _tile_selection_rect.position.x * ts.x;
			tile.position_y = _tile_selection_rect.position.y * ts.y;
			tile.group_size_x = blocky::BLOB9_DEFAULT_LAYOUT_SIZE_X;
			tile.group_size_y = blocky::BLOB9_DEFAULT_LAYOUT_SIZE_Y;

			// TODO UndoRedo
			_atlas->add_tile(tile);
			on_atlas_changed();

		} break;

		case MENU_CREATE_RANDOM_TILE: {
			ZN_ASSERT_RETURN(is_tile_rect_valid(_tile_selection_rect));

			VoxelBlockyTextureAtlas::Tile tile;
			tile.type = blocky::TILE_RANDOM;
			tile.position_x = _tile_selection_rect.position.x * ts.x;
			tile.position_y = _tile_selection_rect.position.y * ts.y;
			tile.group_size_x = _tile_selection_rect.size.x;
			tile.group_size_y = _tile_selection_rect.size.y;

			// TODO UndoRedo
			_atlas->add_tile(tile);
			on_atlas_changed();

		} break;

		case MENU_CREATE_EXTENDED_TILE: {
			ZN_ASSERT_RETURN(is_tile_rect_valid(_tile_selection_rect));

			VoxelBlockyTextureAtlas::Tile tile;
			tile.type = blocky::TILE_EXTENDED;
			tile.position_x = _tile_selection_rect.position.x * ts.x;
			tile.position_y = _tile_selection_rect.position.y * ts.y;
			tile.group_size_x = _tile_selection_rect.size.x;
			tile.group_size_y = _tile_selection_rect.size.y;

			// TODO UndoRedo
			_atlas->add_tile(tile);
			on_atlas_changed();

		} break;

		case MENU_RENAME_TILE:
			open_rename_dialog();
			break;

		case MENU_REMOVE_TILE: {
			remove_selected_tile();
		} break;

		case MENU_GENERATE_FROM_COMPACT5: {
			open_blob9_gen();
		} break;

		default:
			ZN_PRINT_ERROR("Unhandled menu item");
			break;
	}
}

void VoxelBlockyTextureAtlasEditor::remove_selected_tile() {
	const int tile_id = get_selected_tile_id();
	ZN_ASSERT_RETURN(tile_id >= 0);

	// TODO UndoRedo
	_atlas->remove_tile(tile_id);
	on_atlas_changed();
}

void VoxelBlockyTextureAtlasEditor::on_tile_list_item_selected(int item_index) {
	ERR_FAIL_COND(_atlas.is_null());
	const int tile_id = _tile_list->get_item_metadata(item_index);
	set_selected_tile_id(tile_id, false);
}

static void open_popup_at_mouse(PopupMenu *popup, Control *from_control) {
	// Not sure if it has to be that complicated to open a context menu where the mouse is?
	const Vector2 mouse_pos_in_editor =
			from_control->get_screen_position() + from_control->get_local_mouse_position() * from_control->get_scale();
	popup->set_position(mouse_pos_in_editor);
	popup->popup();
}

void VoxelBlockyTextureAtlasEditor::on_tile_list_item_clicked(
		int item_index,
		Vector2 at_position,
		int mouse_button_index
) {
	if (_read_only) {
		return;
	}
	if (static_cast<MouseButton>(mouse_button_index) == ZN_GODOT_MouseButton_RIGHT) {
		ERR_FAIL_COND(_atlas.is_null());
		const int tile_id = _tile_list->get_item_metadata(item_index);
		set_selected_tile_id(tile_id, false);
		update_select_context_menu();
		open_popup_at_mouse(_select_context_menu, _tile_list);
	}
}

int VoxelBlockyTextureAtlasEditor::get_tile_list_index_from_tile_id(const int tile_id_to_find) const {
	const int item_count = _tile_list->get_item_count();
	for (int i = 0; i < item_count; ++i) {
		const int id = _tile_list->get_item_metadata(i);
		if (id == tile_id_to_find) {
			return i;
		}
	}
	return -1;
}

void VoxelBlockyTextureAtlasEditor::set_selected_tile_id(const int tile_id_to_select, const bool update_list) {
	// Don't early-return if the selection comes from the list because that's where we take the selected tile from... a
	// bit ugly
	if (update_list && tile_id_to_select == get_selected_tile_id()) {
		return;
	}

	if (tile_id_to_select == -1) {
		_tile_list->deselect_all();
		set_tile_selection_rect(Rect2i());
		update_inspector(-1);

	} else {
		ERR_FAIL_COND(_atlas.is_null());

		if (update_list) {
			const int i = get_tile_list_index_from_tile_id(tile_id_to_select);
			if (i >= 0) {
				_tile_list->select(i);
				_tile_list->ensure_current_is_visible();
			} else {
				ZN_PRINT_ERROR("Tile not found in list. Bug?");
			}
		}

		const VoxelBlockyTextureAtlas::Tile &tile = _atlas->get_tile(tile_id_to_select);
		const Vector2i ts = _atlas->get_default_tile_resolution();
		set_tile_selection_rect(
				Rect2i(Vector2i(tile.position_x, tile.position_y) / ts, Vector2i(tile.group_size_x, tile.group_size_y))
		);

		update_inspector(tile_id_to_select);
	}
}

void VoxelBlockyTextureAtlasEditor::update_inspector(const int tile_index) {
	_inspector->clear();

	if (tile_index == -1) {
		return;
	}

	ZN_ASSERT_RETURN(tile_index >= 0);
	ZN_ASSERT_RETURN(_atlas.is_valid());

	_inspector->set_target_object(_atlas.ptr());
	_inspector->set_target_index(tile_index);

	// _inspector->add_indexed_property("Name", "set_tile_name", "get_tile_name");
	_inspector->add_indexed_property(
			"Type", "set_tile_type", "get_tile_type", String(VoxelBlockyTextureAtlas::TILE_TYPE_HINT_STRING), false
	);

	_inspector->add_indexed_property(
			"Position",
			"set_tile_position",
			"get_tile_position",
			VoxelBlockyTextureAtlas::get_supported_tile_position_range()
	);

	const VoxelBlockyTextureAtlas::TileType type = _atlas->get_tile_type(tile_index);
	_inspector->add_indexed_property(
			"Group Size",
			"set_tile_group_size",
			"get_tile_group_size",
			VoxelBlockyTextureAtlas::get_supported_tile_group_size_range(),
			type == VoxelBlockyTextureAtlas::TILE_TYPE_EXTENDED || type == VoxelBlockyTextureAtlas::TILE_TYPE_RANDOM
	);

	if (type == VoxelBlockyTextureAtlas::TILE_TYPE_RANDOM) {
		_inspector->add_indexed_property("Random Rotation", "set_tile_random_rotation", "get_tile_random_rotation");
	}
}

void VoxelBlockyTextureAtlasEditor::update_texture_rect() {
	if (_atlas.is_valid()) {
		_texture_rect->set_size(Vector2(_atlas->get_resolution()));
	}
	_texture_rect->queue_redraw();
}

void VoxelBlockyTextureAtlasEditor::update_tile_list() {
	if (_atlas.is_null()) {
		_tile_list->clear();
		return;
	}

	Span<const VoxelBlockyTextureAtlas::Tile> tiles = _atlas->get_tiles();

	// Save selection
	StdVector<int> selected_tile_indices;
	{
		const PackedInt32Array selected_items = _tile_list->get_selected_items();
		const Span<const int32_t> selected_items_s = to_span(selected_items);
		for (const int32_t item_index : selected_items_s) {
			const int tile_id = _tile_list->get_item_metadata(item_index);
			selected_tile_indices.push_back(tile_id);
		}
	}

	// Repopulate list
	_tile_list->clear();
	for (unsigned int tile_index = 0; tile_index < tiles.size(); ++tile_index) {
		const VoxelBlockyTextureAtlas::Tile &tile = tiles[tile_index];
		if (tile.is_tombstone()) {
			continue;
		}

		String item_title;
		if (tile.name.size() == 0) {
			item_title = String("<unnamed{0}>").format(varray(tile_index));
		} else {
			item_title = tile.name.c_str();
		}

		const int item_index = _tile_list->add_item(item_title);
		_tile_list->set_item_metadata(item_index, tile_index);
	}

	// Restore selection
	if (selected_tile_indices.size() > 0) {
		const int item_count = _tile_list->get_item_count();
		for (int item_index = 0; item_index < item_count; ++item_index) {
			const int tile_index = _tile_list->get_item_metadata(item_index);
			if (contains(selected_tile_indices, tile_index)) {
				// Note: `select` does not trigger item selection signals.
				if (selected_tile_indices.size() == 1) {
					_tile_list->select(item_index, true);
					break;
				} else {
					_tile_list->select(item_index, false);
				}
			}
		}
	}
}

void VoxelBlockyTextureAtlasEditor::set_tile_selection_rect_from_pixel_positions(const Vector2 p0, const Vector2 p1) {
	ERR_FAIL_COND(_atlas.is_null());
	const Vector2i ts = _atlas->get_default_tile_resolution();
	ERR_FAIL_COND(ts.x <= 0 || ts.y <= 0);

	const Vector2i pi0(p0);
	const Vector2i pi1(p1);

	const Vector2i minp = math::clamp(math::min(pi0, pi1), Vector2i(), _atlas->get_resolution());
	const Vector2i maxp = math::clamp(math::max(pi0, pi1), Vector2i(), _atlas->get_resolution());

	const Vector2i tminp = minp / ts;
	const Vector2i tmaxp = math::ceildiv(maxp, ts);

	const Vector2i tsize = math::max(tmaxp - tminp, Vector2i(1, 1));

	const Rect2i tile_rect(tminp, tsize);

	set_tile_selection_rect(tile_rect);
}

void VoxelBlockyTextureAtlasEditor::set_tile_selection_rect_from_pixel_position(const Vector2 p0) {
	ERR_FAIL_COND(_atlas.is_null());
	const Vector2i ts = _atlas->get_default_tile_resolution();
	ERR_FAIL_COND(ts.x <= 0 || ts.y <= 0);

	const Vector2i pi0(p0);

	const Vector2i minp = math::clamp(pi0, Vector2i(), _atlas->get_resolution());

	const Vector2i tpos0 = minp / ts;

	const Rect2i tile_rect(tpos0, Vector2i(1, 1));

	set_tile_selection_rect(tile_rect);
}

void VoxelBlockyTextureAtlasEditor::set_tile_selection_rect(const Rect2i rect) {
	if (rect != _tile_selection_rect) {
		_tile_selection_rect = rect;
		_texture_rect->queue_redraw();
	}
}

void VoxelBlockyTextureAtlasEditor::set_hovered_tile_id(const int id) {
	if (id != _hovered_tile_id) {
		_hovered_tile_id = id;
		_texture_rect->queue_redraw();
	}
}

void VoxelBlockyTextureAtlasEditor::set_hovered_tile_position(const Vector2i pos) {
	if (pos != _hovered_tile_position) {
		_hovered_tile_position = pos;
		_texture_rect->queue_redraw();
	}
}

void VoxelBlockyTextureAtlasEditor::on_texture_rect_gui_input(Ref<InputEvent> event) {
	if (_atlas.is_null()) {
		return;
	}

	switch (_mode) {
		case MODE_SELECT: {
			Ref<InputEventMouseMotion> mouse_motion_event = event;
			if (mouse_motion_event.is_valid()) {
				const Vector2 pos = mouse_motion_event->get_position();
				const Vector2i posi = Vector2i(pos);
				const Vector2i ts = _atlas->get_default_tile_resolution();
				set_hovered_tile_id(_atlas->get_tile_id_at_pixel_position(posi));
				return;
			}

			Ref<InputEventMouseButton> mouse_button_event = event;
			if (mouse_button_event.is_valid()) {
				if (mouse_button_event->is_pressed()) {
					switch (mouse_button_event->get_button_index()) {
						case ZN_GODOT_MouseButton_LEFT: {
							if (_hovered_tile_id != -1) {
								set_selected_tile_id(_hovered_tile_id, true);
							}
						} break;

						case ZN_GODOT_MouseButton_RIGHT: {
							if (_hovered_tile_id != -1) {
								set_selected_tile_id(_hovered_tile_id, true);

								if (!_read_only) {
									update_select_context_menu();
									open_popup_at_mouse(_select_context_menu, _texture_rect);
								}
							}
						} break;

						default:
							break;
					}
				}
				return;
			}

			Ref<InputEventKey> key_event = event;
			if (key_event.is_valid()) {
				if (key_event->is_pressed()) {
					if (!_read_only) {
						// TODO Does Godot have anything better to detect the Delete key?? Why is it specific to
						// GraphEdit?
						if (key_event->is_action("ui_graph_delete", true)) {
							remove_selected_tile();
							accept_event();
						}
					}
				}
			}
		} break;

		case MODE_CREATE: {
			Ref<InputEventMouseMotion> mouse_motion_event = event;
			if (mouse_motion_event.is_valid()) {
				const Vector2 pos = mouse_motion_event->get_position();
				const Vector2i posi = Vector2i(pos);
				const Vector2i ts = _atlas->get_default_tile_resolution();
				set_hovered_tile_position(posi / ts);

				const Vector2 mouse_pos = mouse_motion_event->get_position();

				if (_pressed) {
					set_tile_selection_rect_from_pixel_positions(_mouse_press_pos, mouse_pos);
				}

				return;
			}

			Ref<InputEventMouseButton> mouse_button_event = event;
			if (mouse_button_event.is_valid()) {
				if (mouse_button_event->is_pressed()) {
					switch (mouse_button_event->get_button_index()) {
						case ZN_GODOT_MouseButton_LEFT: {
							_mouse_press_pos = mouse_button_event->get_position();
							_pressed = true;
							set_tile_selection_rect_from_pixel_position(_mouse_press_pos);
						} break;

						case ZN_GODOT_MouseButton_RIGHT: {
							const Vector2i mouse_pos_px(mouse_button_event->get_position());
							const int tile_id = _atlas->get_tile_id_at_pixel_position(mouse_pos_px);

							if (tile_id == -1) {
								if (Rect2i(Vector2i(), _atlas->get_resolution()).has_point(mouse_pos_px)) {
									if (_tile_selection_rect == Rect2i()) {
										set_tile_selection_rect(Rect2i(_hovered_tile_position, Vector2i(1, 1)));
									}
									if (!_read_only) {
										open_popup_at_mouse(_create_context_menu, _texture_rect);
									}
								}
							} else {
								_tile_selection_rect = Rect2i();
								// TODO Menu to delete hovered tile?
							}
						} break;

						default:
							break;
					}
				} else {
					_pressed = false;
				}
				return;
			}
		} break;
	}
}

void VoxelBlockyTextureAtlasEditor::on_mode_button_group_pressed(BaseButton *pressed_button) {
	ERR_FAIL_COND(pressed_button == nullptr);
	const int mode = pressed_button->get_meta(VoxelStringNames::get_singleton().mode);
	ERR_FAIL_COND(mode < 0 || mode >= MODE_COUNT);
	_mode = static_cast<Mode>(mode);
	set_tile_selection_rect(Rect2i());
}

void VoxelBlockyTextureAtlasEditor::on_rename_popup_confirmed() {
	const String new_name = _rename_line_edit->get_text().strip_edges();

	if (new_name.is_empty()) {
		ZN_PRINT_ERROR("Name cannot be empty");
		return;
	}

	ZN_ASSERT_RETURN(_atlas.is_valid());

	const int tile_id = get_selected_tile_id();
	ZN_ASSERT_RETURN(tile_id >= 0);

	// TODO UndoRedo
	_atlas->set_tile_name(tile_id, new_name);

	const int item_index = get_tile_list_index_from_tile_id(tile_id);
	_tile_list->set_item_text(item_index, new_name);
}

void VoxelBlockyTextureAtlasEditor::on_connectivity_button_toggled(bool pressed) {
	_texture_rect->queue_redraw();
}

void VoxelBlockyTextureAtlasEditor::on_blob9_gen_margin_spinbox_value_changed(float val) {
	update_blob9_gen();
}

void VoxelBlockyTextureAtlasEditor::on_blob9_gen_apply_button_pressed() {
	ZN_ASSERT_RETURN(_atlas.is_valid());
	ZN_ASSERT_RETURN(_godot_editor_interface != nullptr);

	Ref<Image> image = _blob9_gen.image;
	ZN_ASSERT_RETURN(image.is_valid());

	Ref<Texture2D> texture = _atlas->get_texture();
	ZN_ASSERT_RETURN(texture.is_valid());
	const String res_path = texture->get_path();
	ZN_ASSERT_RETURN(res_path != "");
	// Workaround Godot's annoying warning that "loading images from res:// is bad". It is NOT bad, editor
	// plugins should be able to do it all the time
	String fpath = res_path;
	if (fpath.begins_with("res://")) {
		fpath = res_path.substr(6);
	}

	const String extension = fpath.get_extension();
	ZN_ASSERT_RETURN_MSG(extension == "png" || extension == "webp", "Image file format not supported");

	if (extension == "png") {
		image->save_png(fpath);
	} else if (extension == "webp") {
		image->save_webp(fpath);
	}

	PackedStringArray to_reimport;
	// Note, `res://` paths are required here, otherwise the texture won't update properly
	to_reimport.push_back(res_path);
	EditorFileSystem *efs = zylann::godot::EditorInterfaceShims::get_resource_file_system(*_godot_editor_interface);
	ZN_ASSERT_RETURN(efs != nullptr);
	efs->reimport_files(to_reimport);

	close_blob9_gen();
}

void VoxelBlockyTextureAtlasEditor::on_blob9_gen_cancel_button_pressed() {
	close_blob9_gen();
}

void VoxelBlockyTextureAtlasEditor::open_blob9_gen() {
	ZN_ASSERT_RETURN(_blob9_gen.container->is_visible() == false);
	ZN_ASSERT_RETURN(_atlas.is_valid());

	_blob9_gen.container->show();

	const Vector2i ts = _atlas->get_default_tile_resolution();

	const int tile_id = get_selected_tile_id();
	ZN_ASSERT_RETURN(tile_id != -1);
	const Vector2i saved_margin = _atlas->editor_get_tile_blob9_margin(tile_id);
	const Vector2i margin = saved_margin != Vector2i() ? saved_margin : ts / 3;

	_blob9_gen.margin_x_spinbox->set_min(1);
	_blob9_gen.margin_x_spinbox->set_max(ts.x / 2);
	_blob9_gen.margin_x_spinbox->set_value_no_signal(margin.x);

	_blob9_gen.margin_y_spinbox->set_min(1);
	_blob9_gen.margin_y_spinbox->set_max(ts.y / 2);
	_blob9_gen.margin_y_spinbox->set_value_no_signal(margin.y);

	update_blob9_gen();
}

void VoxelBlockyTextureAtlasEditor::update_blob9_gen() {
	ZN_ASSERT_RETURN(_atlas.is_valid());

	Ref<Texture2D> texture = _atlas->get_texture();
	ZN_ASSERT_RETURN(texture.is_valid());
	const String res_path = texture->get_path();
	ZN_ASSERT_RETURN(res_path != "");
	// Workaround Godot's annoying warning that "loading images from res:// is bad". It is NOT bad, editor
	// plugins should be able to do it all the time
	String fpath = res_path;
	if (fpath.begins_with("res://")) {
		fpath = res_path.substr(6);
	}

	// We reload each time because the workflow can involve the user changing the image too in an external editor
	Ref<Image> image = Image::load_from_file(fpath);
	ZN_ASSERT_RETURN(image.is_valid());

	const int tile_id = get_selected_tile_id();
	ZN_ASSERT_RETURN(tile_id != -1);

	const Vector2i layout_origin = _atlas->get_tile_position(tile_id);
	const Vector2i ts = _atlas->get_default_tile_resolution();

	std::array<Vector2i, blocky::COMPACT5_TILE_COUNT> ref_positions;
	const std::array<uint8_t, blocky::COMPACT5_TILE_COUNT> ref_cases = blocky::get_blob9_reference_cases_for_compact5();
	for (size_t i = 0; i < ref_cases.size(); ++i) {
		const int tx = ref_cases[i] % blocky::BLOB9_DEFAULT_LAYOUT_SIZE_X;
		const int ty = ref_cases[i] / blocky::BLOB9_DEFAULT_LAYOUT_SIZE_X;
		ref_positions[i] = layout_origin + Vector2i(tx, ty) * ts;
	}

	const Vector2i margin(_blob9_gen.margin_x_spinbox->get_value(), _blob9_gen.margin_y_spinbox->get_value());

	blocky::generate_atlas_from_compact5(**image, ts, ref_positions, margin, **image, layout_origin);

	_blob9_gen.image = image;

	if (_blob9_gen.texture.is_null() || _blob9_gen.texture->get_size() != image->get_size()) {
		_blob9_gen.texture = ImageTexture::create_from_image(image);
	} else {
		_blob9_gen.texture->update(image);
	}

	// Save for later to improve UX
	_atlas->editor_set_tile_blob9_margin(tile_id, margin);

	update_texture_rect();
}

void VoxelBlockyTextureAtlasEditor::close_blob9_gen() {
	ZN_ASSERT_RETURN(_blob9_gen.container->is_visible() == true);
	_blob9_gen.image.unref();
	_blob9_gen.texture.unref();
	_blob9_gen.container->hide();
	update_texture_rect();
}

void VoxelBlockyTextureAtlasEditor::on_resources_reimported(PackedStringArray resource_paths) {
	if (_atlas.is_null()) {
		return;
	}
	Ref<Texture> texture = _atlas->get_texture();
	if (texture.is_null()) {
		return;
	}
	if (_blob9_gen.container->is_visible() == false) {
		return;
	}
	const String tex_path = texture->get_path();

	bool changed = false;

	const int count = resource_paths.size();
	for (int i = 0; i < count; ++i) {
		const String path = resource_paths[i];
		if (path == tex_path) {
			changed = true;
			break;
		}
	}

	if (changed) {
		update_blob9_gen();
	}
}

void VoxelBlockyTextureAtlasEditor::_bind_methods() {
	//
}

} // namespace zylann::voxel
