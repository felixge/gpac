/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Copyright (c) Jean Le Feuvre 2000-2005
 *					All rights reserved
 *
 *  This file is part of GPAC / Scene Compositor sub-project
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *   
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 */

#include "nodes_stacks.h"
#include "mpeg4_grouping.h"
#include "visual_manager.h"

typedef struct
{
	PARENT_NODE_STACK_2D
	
	Bool is_scrolling;
	u32 start_scroll_type;
	Double start_time, pause_time;
	GF_List *lines;
	GF_Rect clip;
	Fixed last_scroll, prev_rate, scroll_rate, scale_scroll, scroll_len, scroll_min, scroll_max;
} LayoutStack;

typedef struct
{
	Fixed width, height, ascent, descent;
	u32 first_child, nb_children;
} LineInfo;

static void layout_reset_lines(LayoutStack *st)
{
	while (gf_list_count(st->lines)) {
		LineInfo *li = (LineInfo *)gf_list_get(st->lines, 0);
		gf_list_rem(st->lines, 0);
		free(li);
	}
}

static LineInfo *new_line_info(LayoutStack *st)
{
	LineInfo *li;
	GF_SAFEALLOC(li, LineInfo);

	gf_list_add(st->lines, li);
	return li;
}
static GFINLINE LineInfo *get_line_info(LayoutStack *st, u32 i)
{
	return (LineInfo *) gf_list_get(st->lines, i);
}


enum
{
	L_FIRST,
	L_BEGIN,
	L_MIDDLE,
	L_END,
	L_JUSTIFY,
};

static u32 get_justify(M_Layout *l, u32 i)
{
	if (l->justify.count <= i) return L_BEGIN;
	if (!strcmp(l->justify.vals[i], "END")) return L_END;
	if (!strcmp(l->justify.vals[i], "MIDDLE")) return L_MIDDLE;
	if (!strcmp(l->justify.vals[i], "FIRST")) return L_FIRST;
	if (!strcmp(l->justify.vals[i], "SPREAD")) return L_JUSTIFY;
	if (!strcmp(l->justify.vals[i], "JUSTIFY")) return L_JUSTIFY;
	return L_BEGIN;
}

static void get_lines_info(LayoutStack *st, M_Layout *l)
{
	Fixed prev_discard_width;
	u32 i, count;
	LineInfo *li;
	Fixed max_w, max_h;

	max_w = st->clip.width;
	max_h = st->clip.height;
	layout_reset_lines(st);
	
	count = gf_list_count(st->groups);
	if (!count) return;

	li = new_line_info(st);
	li->first_child = 0;
	prev_discard_width = 0;

	for (i=0; i<count; i++) {
		ChildGroup *cg = (ChildGroup *)gf_list_get(st->groups, i);
		if (!l->horizontal) {
			/*check if exceed column size or not - if so, move to next column or clip given wrap mode*/
			if (cg->final.height + li->height > max_h) {
				if (l->wrap) {
					li = new_line_info(st);
					li->first_child = i;
				}
			}
			if (cg->final.width > li->width) li->width = cg->final.width;
			li->height += cg->final.height;
			li->nb_children ++;
		} else {
			if (i && (cg->final.width + li->width> max_w)) {
				if (l->wrap) {
					if (!li->ascent) {
						li->ascent = li->height;
						li->descent = 0;
					}
					/*previous word is discardable (' ')*/
					if (prev_discard_width) {
						li->width -= prev_discard_width;
						li->nb_children--;
					}
					if (cg->discardable && (i+1==count)) break; 

					li = new_line_info(st);
					li->first_child = i;
					if (cg->discardable) {
						li->first_child++;
						continue;
					}
				} 
			}
		
			/*get ascent/descent for text or height for non-text*/
			if (cg->ascent) {
				if (li->ascent < cg->ascent) li->ascent = cg->ascent;
				if (li->descent < cg->descent) li->descent = cg->descent;
				if (li->height < li->ascent + li->descent) li->height = li->ascent + li->descent;
			} else if (cg->final.height > li->height) {
				li->height = cg->final.height;
			}
			li->width += cg->final.width;
			li->nb_children ++;

			prev_discard_width = cg->discardable ? cg->final.width : 0;

		}
	}
}


static void layout_justify(LayoutStack *st, M_Layout *l)
{
	u32 first, minor, major, i, k, nbLines;
	Fixed current_top, current_left, h;
	LineInfo *li;
	ChildGroup *cg, *prev;
	get_lines_info(st, l);
	major = get_justify(l, 0);
	minor = get_justify(l, 1);
	
	st->scroll_len = 0;
	nbLines = gf_list_count(st->lines);
	if (l->horizontal) {
		if (l->wrap && !l->topToBottom) {
			li = (LineInfo*)gf_list_get(st->lines, 0);
			current_top = st->clip.y - st->clip.height;
			if (li) current_top += li->height;
		} else {
			current_top = st->clip.y;
		}

		/*for each line perform adjustment*/
		for (k=0; k<nbLines; k++) {
			Fixed spacing = 0;
			li = (LineInfo*)gf_list_get(st->lines, k);
			first = li->first_child;
			if (!l->leftToRight) first += li->nb_children - 1;

			if (!l->topToBottom && k) current_top += li->height;
			
			/*set major alignment (X) */
			cg = (ChildGroup *)gf_list_get(st->groups, first);
			switch (major) {
			case L_END:
				cg->final.x = st->clip.x + st->clip.width - li->width;
				break;
			case L_MIDDLE:
				cg->final.x = st->clip.x + (st->clip.width - li->width)/2;
				break;
			case L_FIRST:
			case L_BEGIN:
				cg->final.x = st->clip.x;
				break;
			case L_JUSTIFY:
				cg->final.x = st->clip.x;
				if (li->nb_children>1) {
					cg = (ChildGroup *)gf_list_get(st->groups, li->nb_children-1);
					spacing = (st->clip.width - li->width) / (li->nb_children-1) ;
					if (spacing<0) spacing = 0;
					/*disable spacing for last text line*/
					else if (cg->ascent && (k+1==nbLines) ) 
						spacing = 0;
					
				}
				break;
			}          


			/*for each in the run */
			i = first;
			while (1) {
				cg = (ChildGroup *)gf_list_get(st->groups, i);
				h = MAX(li->ascent, li->height);
				switch (minor) {
				case L_FIRST:
					cg->final.y = current_top - h;
					if (cg->ascent) {
						cg->final.y += cg->ascent;
					} else {
						cg->final.y += cg->final.height;
					}
					break;
				case L_MIDDLE:
					cg->final.y = current_top - (h - cg->final.height)/2;
					break;
				case L_END:
					cg->final.y = current_top;
					break;
				case L_BEGIN:
				default:
					cg->final.y = current_top - h + cg->final.height;
					break;
				}
				/*update left for non-first children in line*/
				if (i != first) {
					if (l->leftToRight) {
						prev = (ChildGroup *)gf_list_get(st->groups, i-1);
					} else {
						prev = (ChildGroup *)gf_list_get(st->groups, i+1);
					}
					cg->final.x = prev->final.x + prev->final.width + spacing;
				}
				i += l->leftToRight ? +1 : -1;
				if (l->leftToRight && (i==li->first_child + li->nb_children))
					break;
				else if (!l->leftToRight && (i==li->first_child - 1))
					break;		
			}
			if (l->topToBottom) {
				current_top -= gf_mulfix(l->spacing, li->height);
			} else {
				current_top += gf_mulfix(l->spacing - FIX_ONE, li->height);
			}
			if (l->scrollVertical) {
				st->scroll_len += li->height;
			} else {
				if (st->scroll_len < li->width) st->scroll_len = li->width;
			}
		}
		return;
	}

	/*Vertical aligment*/
	li = (LineInfo*)gf_list_get(st->lines, 0);
	if (l->wrap && !l->leftToRight) {
		current_left = st->clip.x + st->clip.width;
		if (li) current_left -= li->width;
	} else {
		current_left = st->clip.x;
	}

	/*for all columns in run*/
	for (k=0; k<nbLines; k++) {
		Fixed spacing = 0;
		li = (LineInfo*)gf_list_get(st->lines, k);

		first = li->first_child;
		if (!l->topToBottom) first += li->nb_children - 1;
		
		/*set major alignment (Y) */
		cg = (ChildGroup *)gf_list_get(st->groups, first);
		switch (major) {
		case L_END:
			cg->final.y = st->clip.y - st->clip.height + li->height;
			break;
		case L_MIDDLE:
			cg->final.y = st->clip.y - st->clip.height/2 + li->height/2;
			break;
		case L_FIRST:
		case L_BEGIN:
			cg->final.y = st->clip.y;
			break;
		case L_JUSTIFY:
			cg->final.y = st->clip.y;
			if (li->nb_children>1) {
				spacing = (st->clip.height - li->height) / (li->nb_children-1) ;
				if (spacing<0) spacing = 0;
			}
			break;
		}          

		/*for each in the run */
		i = first;
		while (1) {
			cg = (ChildGroup *)gf_list_get(st->groups, i);
			switch (minor) {
			case L_MIDDLE:
				cg->final.x = current_left + li->width/2 - cg->final.width/2;
				break;
			case L_END:
				cg->final.x = current_left + li->width - cg->final.width;
				break;
			case L_BEGIN:
			case L_FIRST:
			default:
				cg->final.x = current_left;
				break;
			}
			/*update top for non-first children in line*/
			if (i != first) {
				if (l->topToBottom) {
					prev = (ChildGroup *)gf_list_get(st->groups, i-1);
				} else {
					prev = (ChildGroup *)gf_list_get(st->groups, i+1);
				}
				cg->final.y = prev->final.y - prev->final.height + spacing;
			}
			i += l->topToBottom ? +1 : -1;
			if (l->topToBottom && (i==li->first_child + li->nb_children))
				break;
			else if (!l->topToBottom && (i==li->first_child - 1))
				break;		
		}
		if (l->leftToRight) {
			current_left += gf_mulfix(l->spacing, li->width);
		} else if (k < nbLines - 1) {
			li = (LineInfo*)gf_list_get(st->lines, k+1);
			current_left -= gf_mulfix(l->spacing, li->width);
		}
		if (l->scrollVertical) {
			if (st->scroll_len < li->height) st->scroll_len = li->height;
		} else {
			st->scroll_len += li->width;
		}
	}		
}

static void layout_setup_scroll_bounds(LayoutStack *st, M_Layout *l)
{
	u32 minor_justify = 0;

	st->scroll_min = st->scroll_max = 0;

	if (l->horizontal) minor_justify = l->scrollVertical ? 1 : 0;
	else minor_justify = l->scrollVertical ? 0 : 1;

	/*update scroll-out max limit*/
	if (l->scrollMode != -1) {
		/*set max limit*/
		switch( get_justify(l, minor_justify)) {
		case L_END:
			if (l->scrollVertical) {
				if (st->scale_scroll<0) st->scroll_max = - st->scroll_len;
				else st->scroll_max = st->clip.height;
			} else {
				if (st->scale_scroll<0) st->scroll_max = - st->clip.width;
				else st->scroll_max = st->scroll_len;
			}
			break;
		case L_MIDDLE:
			if (l->scrollVertical) {
				if (st->scale_scroll<0) st->scroll_max = - (st->clip.height + st->scroll_len)/2;
				else st->scroll_max = (st->clip.height + st->scroll_len)/2;
			} else {
				if (st->scale_scroll<0) st->scroll_max = - (st->clip.width + st->scroll_len)/2;
				else st->scroll_max = (st->clip.width + st->scroll_len)/2;
			}
			break;
		default:
			if (l->scrollVertical) {
				if (st->scale_scroll<0) st->scroll_max = - st->clip.height;
				else st->scroll_max = st->scroll_len;
			} else {
				if (st->scale_scroll<0) st->scroll_max = - st->scroll_len;
				else st->scroll_max = st->clip.width;
			}
			break;
		}
	} 
	/*scroll-in only*/
	else {
		st->scroll_max = 0;
	}

	/*scroll-out only*/
	if (l->scrollMode==1) {
		st->scroll_min = 0;
		return;
	}

	/*when vertically scrolling an horizontal layout, don't use vertical justification, only justify top/bottom lines*/
	if (l->horizontal && l->scrollVertical) {
		if (st->scale_scroll<0) {
			st->scroll_min = st->scroll_len;
		} else {
			st->scroll_min = - st->clip.height;
		}
		return;
	}

	/*update scroll-in offset*/
	switch( get_justify(l, minor_justify)) {
	case L_END:
		if (l->scrollVertical) {
			if (st->scale_scroll<0) st->scroll_min = st->clip.height;
			else st->scroll_min = - st->scroll_len;
		} else {
			if (st->scale_scroll<0) st->scroll_min = st->scroll_len;
			else st->scroll_min = -st->clip.width;
		}
		break;
	case L_MIDDLE:
		if (l->scrollVertical) {
			if (st->scale_scroll<0) st->scroll_min = (st->clip.height + st->scroll_len)/2;
			else st->scroll_min = - (st->clip.height + st->scroll_len)/2;
		} else {
			if (st->scale_scroll<0) st->scroll_min = (st->clip.width + st->scroll_len)/2;
			else st->scroll_min = - (st->clip.width + st->scroll_len)/2;
		}
		break;
	default:
		if (l->scrollVertical) {
			if (st->scale_scroll<0) st->scroll_min = st->scroll_len;
			else st->scroll_min = - st->clip.height;
		} else {
			if (st->scale_scroll<0) st->scroll_min = st->clip.width;
			else st->scroll_min = - st->scroll_len;
		}
		break;
	}
}


static void layout_scroll(GF_TraverseState *tr_state, LayoutStack *st, M_Layout *l)
{
	u32 i, nb_lines;
	Fixed scrolled, rate, ellapsed, scroll_diff;
	Bool smooth, do_scroll, stop;
	Double time;
	ChildGroup *cg;

	/*not scrolling*/
	if (!st->scale_scroll && !st->is_scrolling) return;

	time = gf_node_get_scene_time((GF_Node *)l);

//	if (st->scale_scroll && (st->prev_rate!=st->scale_scroll)) st->start_scroll_type = 1;

	/*if scroll rate changed to previous non-zero value, this is a 
	scroll restart, don't re-update bounds*/
	if ((st->start_scroll_type==2) && (st->prev_rate==st->scale_scroll)) st->start_scroll_type = 0;
	
	if (st->start_scroll_type) {
		st->start_time = time;
		st->is_scrolling = 1;
		st->prev_rate = st->scale_scroll;

		/*continuous restart: use last scroll to update the start time. We must recompute scroll bounds
		since switching from scroll_rate >0 to <0 changes the bounds !*/
		if ((st->start_scroll_type==2) && st->scale_scroll) {
			Fixed cur_pos = st->scroll_min + st->last_scroll;
			layout_setup_scroll_bounds(st, l);
			cur_pos -= st->scroll_min;
			st->start_time = time - FIX2FLT(gf_divfix(cur_pos, st->scale_scroll));
		} else {
			layout_setup_scroll_bounds(st, l);
		}
		st->last_scroll = 0;
		st->start_scroll_type = 0;
	}

	/*handle pause/resume*/
	rate = st->scale_scroll;
	if (!rate) {
		if (!st->pause_time) {
			st->pause_time = time;
		} else {
			time = st->pause_time;
		}
		rate = st->prev_rate;
	} else if (st->pause_time) {
		st->start_time += (time - st->pause_time);
		st->pause_time = 0;
	}

	smooth = l->smoothScroll;
	/*if the scroll is in the same direction as the layout, there is no notion of line or column to scroll
	so move to smooth mode*/
	if (!l->horizontal && l->scrollVertical) smooth = 1;
	else if (l->horizontal && !l->scrollVertical) smooth = 1;

	/*compute advance in pixels for smooth scroll*/
	ellapsed = FLT2FIX((Float) (time - st->start_time));
	scrolled = gf_mulfix(ellapsed, rate);

	stop = 0;
	scroll_diff = st->scroll_max - st->scroll_min;
	if ((scroll_diff<0) && (scrolled<scroll_diff)) {
		stop = 1;
		scrolled = scroll_diff;
	}
	else if ((scroll_diff>0) && (scrolled>scroll_diff)) {
		stop = 1;
		scrolled = scroll_diff;
	}
	
	do_scroll = 1;
	if (!stop) {
		if (smooth) {
			do_scroll = 1;
		} else {
			scroll_diff = scrolled - st->last_scroll;
			do_scroll = 0;

			nb_lines = gf_list_count(st->lines);
			for (i=0; i < nb_lines; i++) {
				LineInfo *li = (LineInfo*)gf_list_get(st->lines, i);
				if (l->scrollVertical) {
					if (ABS(scroll_diff) >= li->height) {
						do_scroll = 1;
						break;
					}
				} else {
					if (fabs(scroll_diff) >= li->width) {
						do_scroll = 1;
						break;
					}
				}
			}
		}
	}

	if (do_scroll) 
		st->last_scroll = scrolled;
	else
		scrolled = st->last_scroll;

	i=0;
	while ((cg = (ChildGroup *)gf_list_enum(st->groups, &i))) {
		if (l->scrollVertical) {
			cg->scroll_y = st->scroll_min + scrolled;
			cg->scroll_x = 0;
		} else {
			cg->scroll_x = st->scroll_min + scrolled;
			cg->scroll_y = 0;
		}
	}

	/*draw next frame*/
	if (!stop) {
		gf_sc_invalidate(tr_state->visual->compositor, NULL);
		return;
	}

	/*done*/
	if (!l->loop) return;

	/*restart*/
	st->start_time = time;
	gf_sc_invalidate(tr_state->visual->compositor, NULL);
}


static void TraverseLayout(GF_Node *node, void *rs, Bool is_destroy)
{
	Bool recompute_layout;
	u32 i;
	ChildGroup *cg;
	GF_IRect prev_clip;
	Bool mode_bckup, had_clip;
	ParentNode2D *parent_bck;
	GF_Rect clip, prev_clipper;
	M_Layout *l = (M_Layout *)node;
	LayoutStack *st = (LayoutStack *) gf_node_get_private(node);
	GF_TraverseState *tr_state = (GF_TraverseState *)rs;

	if (is_destroy) {
		layout_reset_lines(st);
		parent_node_predestroy((ParentNode2D *)st);
		gf_list_del(st->lines);
		free(st);
		return;
	}
	
	/*note we don't clear dirty flag, this is done in traversing*/
	if (gf_node_dirty_get(node) & GF_SG_NODE_DIRTY) {

		/*TO CHANGE IN BIFS - scroll_rate is quite unusable*/
		st->scale_scroll = st->scroll_rate = l->scrollRate;
		/*move to pixel metrics*/
		if (visual_get_size_info(tr_state, &st->clip.width, &st->clip.height)) {
			st->scale_scroll = gf_mulfix(st->scale_scroll, l->scrollVertical ? st->clip.height : st->clip.width);
		}
		/*setup bounds in local coord system*/
		if (l->size.x>=0) st->clip.width = l->size.x;
		if (l->size.y>=0) st->clip.height = l->size.y;
		st->bounds = st->clip = gf_rect_center(st->clip.width, st->clip.height);

		if (st->scale_scroll && !st->start_scroll_type) st->start_scroll_type = 1;
	}

	/*don't waste time traversing is pick ray not in clipper*/
	if ((tr_state->traversing_mode==TRAVERSE_PICK) && !gf_sc_pick_in_clipper(tr_state, &st->clip)) 
		goto layout_exit;

	if (tr_state->traversing_mode==TRAVERSE_GET_BOUNDS) {
		tr_state->bounds = st->clip;
#ifndef GPAC_DISABLE_3D
		gf_bbox_from_rect(&tr_state->bbox, &st->clip);
#endif
		goto layout_exit;
	}

	recompute_layout = 0;
	if (gf_node_dirty_get(node)) {
		recompute_layout = 1;
		gf_node_dirty_clear(node, 0);
	}

	/*setup clipping*/
	prev_clip = tr_state->visual->top_clipper;
	if (tr_state->traversing_mode==TRAVERSE_SORT) {
		clip = compositor_2d_update_clipper(tr_state, st->clip, &had_clip, &prev_clipper, 0);
		if (tr_state->has_clip) {
			tr_state->visual->top_clipper = gf_rect_pixelize(&tr_state->clipper);
			gf_irect_intersect(&tr_state->visual->top_clipper, &prev_clip);
		}
	}
	if (recompute_layout) {
		GF_LOG(GF_LOG_COMPOSE, GF_LOG_DEBUG, ("[Layout] recomputing positions\n"));

		parent_node_reset((ParentNode2D*)st);

		/*setup traversing state*/
		parent_bck = tr_state->parent;
		mode_bckup = tr_state->traversing_mode;
		tr_state->traversing_mode = TRAVERSE_GET_BOUNDS;
		tr_state->parent = (ParentNode2D *) st;

		if (l->wrap) tr_state->text_split_mode = 1;
		parent_node_traverse(node, (ParentNode2D *)st, tr_state);
		/*restore traversing state*/
		tr_state->parent = parent_bck;
		tr_state->traversing_mode = mode_bckup;
		if (l->wrap) tr_state->text_split_mode = 0;

		/*center all nodes*/
		i=0;
		while ((cg = (ChildGroup *)gf_list_enum(st->groups, &i))) {
			cg->final.x = - cg->final.width/2;
			cg->final.y = cg->final.height/2;
		}

		/*apply justification*/
		layout_justify(st, l);

		/*prepare initial scroll bounds*/
//		layout_setup_scroll_bounds(st, l);
	}

	/*scroll*/
	layout_scroll(tr_state, st, l);

	i=0;
	while ((cg = (ChildGroup *)gf_list_enum(st->groups, &i))) {
		parent_node_child_traverse(cg, tr_state);
	}
	tr_state->visual->top_clipper = prev_clip;
	if (tr_state->traversing_mode==TRAVERSE_SORT)  {
		if (had_clip) tr_state->clipper = prev_clipper;
		tr_state->has_clip = had_clip;
	}

layout_exit:
	tr_state->text_split_mode = 0;
}

void compositor_init_layout(GF_Compositor *compositor, GF_Node *node)
{
	LayoutStack *stack;
	GF_SAFEALLOC(stack, LayoutStack);

	parent_node_setup((ParentNode2D*)stack);
	stack->lines = gf_list_new();
	gf_node_set_private(node, stack);
	gf_node_set_callback_function(node, TraverseLayout);
}

void compositor_layout_modified(GF_Compositor *compositor, GF_Node *node)
{
	LayoutStack *st = (LayoutStack *) gf_node_get_private(node);
	/*if modif other than scrollrate restart scroll*/
	if (st->scroll_rate == ((M_Layout*)node)->scrollRate) {
		st->start_scroll_type = 1;
	} 
	/*if modif on scroll rate only, indicate continous restart*/
	else if (((M_Layout*)node)->scrollRate) {
		st->start_scroll_type = 2;
	}
	gf_node_dirty_set(node, GF_SG_NODE_DIRTY, 0);
	gf_sc_invalidate(compositor, NULL);
}

