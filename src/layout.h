#pragma once

#include "monitor.h"
#include "client.h"

extern const Layout layouts[];
extern const unsigned int layout_count;

void dwindle(Monitor *m);
void master(Monitor *m);
void monocle(Monitor *m);

[[nodiscard]] int current_tag_idx(Monitor *m);
[[nodiscard]] const Layout *curlayout(Monitor *m);
void ensure_cold(Monitor *m);

[[nodiscard]] int dwindle_find_leaf(const DwindleTree *tree, Client *c);
void dwindle_recalc(DwindleTree *tree, int gap);
void dwindle_free_tree(DwindleTree *tree);
void dwindle_remove_client(Client *c);
void master_remove_client(Client *c);
void swaptiled(Client *a, Client *b);
