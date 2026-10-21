/*
BGH LIVE Front
Copyright (C) 2026 Barrington Gospel Hall

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/
// The presentation looks the same in every scene. The slideshow is shown in OBS by a screen or window capture,
// and the same capture is in several scenes (the slideshow scenes and the camera scenes). If it's cropped or
// placed differently in each, the slideshow jumps in size when the scene changes. So when the capture is moved,
// resized or cropped in one scene, the plug-in makes it the same in every other scene it's in.
//
// Checked a few times a second on OBS's UI thread: for each capture in more than one scene, the copy whose
// position or crop just changed is the one that was edited, and the others are set to match it. A capture newly
// added to a scene takes the size it already has elsewhere.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QTimer>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "front.h"

namespace front {
namespace {

// Sources that show a screen or a window: where the slideshow comes from
bool isCapture(obs_source_t *s)
{
	const char *id = s ? obs_source_get_unversioned_id(s) : nullptr;
	if (!id)
		return false;
	for (const char *c : {"monitor_capture", "window_capture", "game_capture", "display_capture", "screen_capture",
			      "xcomposite_input", "pipewire-screen-capture-source", "pipewire-window-capture-source"})
		if (strcmp(id, c) == 0)
			return true;
	return false;
}

struct Look {
	obs_transform_info t{};
	obs_sceneitem_crop c{};
};

bool same(const Look &a, const Look &b)
{
	const obs_transform_info &x = a.t, &y = b.t;
	return x.pos.x == y.pos.x && x.pos.y == y.pos.y && x.rot == y.rot && x.scale.x == y.scale.x &&
	       x.scale.y == y.scale.y && x.alignment == y.alignment && x.bounds_type == y.bounds_type &&
	       x.bounds_alignment == y.bounds_alignment && x.bounds.x == y.bounds.x && x.bounds.y == y.bounds.y &&
	       x.crop_to_bounds == y.crop_to_bounds && a.c.left == b.c.left && a.c.top == b.c.top &&
	       a.c.right == b.c.right && a.c.bottom == b.c.bottom;
}

struct Item {
	std::string key; // the scene's UUID and the item's id: stays the same while the item exists
	obs_sceneitem_t *item = nullptr;
	obs_source_t *source = nullptr;
	Look look;
};

std::map<std::string, Look> g_last; // each copy as it was at the last check
QTimer *g_timer = nullptr;

void check()
{
	std::vector<Item> items;
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *sceneSrc = scenes.sources.array[i];
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		if (!scene)
			continue;
		struct Ctx {
			std::vector<Item> *items;
			std::string uuid;
		} ctx{&items, obs_source_get_uuid(sceneSrc)};
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
				auto *c = static_cast<Ctx *>(p);
				obs_source_t *s = obs_sceneitem_get_source(item);
				if (!isCapture(s))
					return true;
				Item it;
				it.key = c->uuid + "#" + std::to_string(obs_sceneitem_get_id(item));
				obs_sceneitem_addref(item);
				it.item = item;
				it.source = s;
				obs_sceneitem_get_info2(item, &it.look.t);
				obs_sceneitem_get_crop(item, &it.look.c);
				c->items->push_back(it);
				return true;
			},
			&ctx);
	}

	std::map<std::string, Look> now;
	std::map<obs_source_t *, std::vector<Item *>> bySource;
	for (auto &it : items)
		bySource[it.source].push_back(&it);
	for (auto &[src, group] : bySource) {
		if (group.size() > 1) {
			// The copy that was just edited leads; failing that, a copy that was already there leads a new one
			Item *leader = nullptr;
			bool added = false;
			for (Item *it : group) {
				auto f = g_last.find(it->key);
				if (f == g_last.end())
					added = true;
				else if (!leader && !same(f->second, it->look))
					leader = it;
			}
			if (!leader && added)
				for (Item *it : group)
					if (g_last.count(it->key)) {
						leader = it;
						break;
					}
			if (leader) {
				int changed = 0;
				for (Item *it : group) {
					if (it == leader || same(it->look, leader->look))
						continue;
					obs_sceneitem_defer_update_begin(it->item);
					obs_sceneitem_set_info2(it->item, &leader->look.t);
					obs_sceneitem_set_crop(it->item, &leader->look.c);
					obs_sceneitem_defer_update_end(it->item);
					it->look = leader->look;
					changed++;
				}
				if (changed)
					obs_log(LOG_INFO, "presentation: \"%s\" now has the same size and crop in %d more scene%s",
						obs_source_get_name(src), changed, changed == 1 ? "" : "s");
			}
		}
		for (Item *it : group)
			now[it->key] = it->look;
	}
	g_last.swap(now);

	for (auto &it : items)
		obs_sceneitem_release(it.item);
	obs_frontend_source_list_free(&scenes);
}

} // namespace

void syncStart()
{
	if (g_timer)
		return;
	g_last.clear();
	check(); // remembers how each copy looks now, so the first edit after this is the one that's copied
	g_timer = new QTimer();
	g_timer->setInterval(300);
	QObject::connect(g_timer, &QTimer::timeout, []() { check(); });
	g_timer->start();
}

void syncStop()
{
	if (!g_timer)
		return;
	g_timer->stop();
	delete g_timer;
	g_timer = nullptr;
	g_last.clear();
}

} // namespace front
