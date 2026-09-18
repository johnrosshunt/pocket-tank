/* setup.h — first-run setup (2026-09-13): the welcome, a name and colours for
 * each of the two fry, then the care tips - for a fresh install and after a
 * reset. Platform-agnostic: the state machine, the gestures, the hit
 * geometry and the drawing all live here (the reset prompt kept its state in
 * each platform's touch code; a five-page flow would have been written
 * twice), built on render.h's UI primitives over the LIVE tank - the fish
 * keep swimming behind the pages and wear the colours as they are picked.
 *
 * The platforms do three things: setup_begin when progression says the flow
 * is pending (boot, a reset YES, the director's cue); setup_touch every
 * frame with the finger's position and state while it is up (taps and the
 * letter-wheel drags are classified here); and render_setup after
 * render_tank while setup_active. Every other gesture is swallowed while it
 * is up. No timeout: it waits for the keeper.
 *
 * PLACE THE BUBBLES (2026-09-13, Strato's idea): right after the welcome,
 * over the live tank - a finger anywhere on the water drags the bubble
 * column to that x (tank_set_bubble_x keeps it off the reef rock); a translucent stripe marks the column, the airstone at its foot
 * moves with it, the bubbles already rising shift as one.
 *
 * Naming (third design, Strato: "seeing the fish while naming it is very
 * important" - and a keyboard's keys are too small for a 35 mm glass): NO
 * panel - the tank stays in view with a ring around the fish being named -
 * and seven big letter slots across the middle, arcade high-score style.
 * Touch a slot to pick it, then drag up / down on it (or tap the chevrons
 * above and below) to spin its letter through blank, A..Z. A blank at the
 * end shortens the name; NEXT trims blanks and puts the preset's name back
 * if nothing is left. Targets are a slot wide (44 px) and the whole band
 * tall, so nothing here needs aim.
 *
 * THE BIRTH FLOW (2026-09-14, Strato: an arrival "will be an exciting event
 * for the player"): when progression says a fry is owed its welcome
 * (progression_newborn), setup_poll_birth opens three pages over the live
 * tank - A NEW FRY! (the announcement: the fry ringed wherever it hatched,
 * its parents named), NAME THE NEW FRY (the letter wheel, as in the first
 * run) and the family page (a panel: the fry's portrait on its own, as it
 * is now; whose body and whose markings it wears; its boldness and
 * sociability on bars between its parents' marks). No colour picker - a
 * fry's look is its family's (tank_add_fish). DONE clears the debt and
 * saves the name; a cancel (director `setup off`, sim S) leaves it owed, so
 * it returns at the next boot. */
/* PLACE A PIECE (2026-09-16, Strato: a bought plant or decoration "should
 * allow the player to place the piece wherever they like ... similar to the
 * bubble column placement experience", plus a z-layer): one page over the
 * live tank, opened by the platform right after a purchase (and by the
 * shop's MOVE button for a piece already in the tank). A finger on the
 * water drags the piece to that x (tank_decor_set clamps it inside the
 * window); a BACK / MIDDLE / FRONT row under the title picks its depth -
 * the tank redraws live, so the fish and the grass show the choice at
 * once; DONE saves. A translucent stripe marks the piece's footprint, a
 * chevron at its foot. */
#ifndef POCKET_TANK_SETUP_H
#define POCKET_TANK_SETUP_H
#include "tank.h"
#include <stdint.h>
#include <stdbool.h>

void setup_begin(tank_t *t);             /* page 1 (needs 2 fish; fewer = done at once) */
void setup_begin_birth(tank_t *t, int slot);   /* the birth flow for the fry in `slot` */
/* platforms, every frame while no prompt owns the glass: opens the birth
 * flow once per arrival owed (never over a flow already up); returns the
 * slot it opened for, or -1 */
int  setup_poll_birth(tank_t *t);
void setup_begin_place(tank_t *t, int item);   /* the placement page for SD item `item` (placeable ones only) */
bool setup_active(void);
bool setup_is_birth(void);               /* the birth flow, not the first run */
bool setup_is_place(void);               /* the placement page */
int  setup_item(void);                   /* the placement page's SD item (-1 = none) */
int  setup_fish(void);                   /* the fish the current page is about (-1 = none) */
void setup_cancel(tank_t *t);            /* drop the pages; the save still says pending */
/* the name page's two REJECTED designs of 2026-09-13, kept to be shown (the
 * episode 6 shoot; director `kbd`): 1 = the first cut, a 7 x 4 grid of 46 x 34
 * px keys on a panel with BACK / NEXT under it ("rarely" hittable, DEL kept
 * landing on NEXT); 2 = the second, 5 x 3 keys of 66 x 74 px over half the
 * alphabet, a corner key flipping A-M / N-Z, the nav on top (hittable, but the
 * panel still hid the fish). 0 = the letter wheel, the design that shipped.
 * Not saved: a boot is always the wheel. */
enum { SETUP_KBD_WHEEL, SETUP_KBD_GRID, SETUP_KBD_PAGES };
void setup_set_keyboard(int mode);
int  setup_keyboard(void);
int  setup_page(void);                   /* SETUP_PG_* while active */
/* the finger, every frame (or poll): x,y in tank coordinates, down = touching.
 * A press then release on one element taps it; a press on a letter slot and
 * a vertical drag spins that letter (SETUP_SPIN_PX of travel per step). */
void setup_touch(tank_t *t, float x, float y, bool down);
/* the pieces, for tests and the touch log: the element under (x,y) on the
 * current page (0 = none) and a tap on it */
int  setup_hit(float x, float y);
void setup_activate(tank_t *t, int id);
const char *setup_hit_name(int id);      /* "NEXT", "slot 2", "up", "body 3"... */
int  setup_slot(void);                   /* the active letter slot on a name page */
void render_setup(const tank_t *t, uint16_t *fb, int stride, float clock);

/* pages, in order */
enum { SETUP_PG_WELCOME, SETUP_PG_BUBBLES, SETUP_PG_NAME_A, SETUP_PG_LOOK_A, SETUP_PG_NAME_B, SETUP_PG_LOOK_B, SETUP_PG_CARE, SETUP_PG_N };
/* the birth flow's pages, in order (numbered past the first run's) */
enum { SETUP_PG_BORN = SETUP_PG_N, SETUP_PG_NAME_NEW, SETUP_PG_FAMILY, SETUP_PG_BIRTH_END };
#define SETUP_BIRTH_PAGES (SETUP_PG_BIRTH_END - SETUP_PG_BORN)
/* the placement page (its own one-page flow, numbered past the birth flow's) */
enum { SETUP_PG_PLACE = SETUP_PG_BIRTH_END };
/* element ids (setup_hit / setup_activate) */
#define SETUP_HIT_NEXT  1
#define SETUP_HIT_BACK  2
#define SETUP_HIT_UP    3                /* the active slot's letter: next (A -> B) */
#define SETUP_HIT_DOWN  4                /* ... previous */
#define SETUP_HIT_SLOT0 10               /* + 0..FISH_NAME_MAX-1: pick that slot */
#define SETUP_HIT_BODY0 40               /* + swatch 0..LOOK_N-1 */
#define SETUP_HIT_KEY0  100              /* legacy keyboards only: + 0..25 = A..Z, + SETUP_KEY_DEL, + SETUP_KEY_PAGE */
#define SETUP_KEY_DEL   26
#define SETUP_KEY_PAGE  27
#define SETUP_KEY_N     28
#define SETUP_HIT_Z0    60               /* + DECOR_Z_BACK..FRONT: the placement page's layer row */

/* geometry (tank coordinates), shared by the drawing, the hit test and the
 * sim's selftest. The panelled pages (welcome, care, family) are a 400 x 400
 * panel centred on the square tank (2026-09-17; the 1.8's was 384 x 326);
 * the name, colour, bubbles and placement pages draw straight on the tank
 * with their title and buttons along the top (SETUP_TOP_Y). */
#define SETUP_W 400
#define SETUP_H 400
#define SETUP_X ((TANK_W - SETUP_W) / 2)
#define SETUP_Y ((TANK_H - SETUP_H) / 2)
#define SETUP_BTN_W 120
#define SETUP_BTN_H 44
#define SETUP_BTN_Y (SETUP_Y + SETUP_H - 12 - SETUP_BTN_H)   /* welcome / care: the foot */
#define SETUP_BACK_X (SETUP_X + 16)
#define SETUP_NEXT_X (SETUP_X + SETUP_W - 16 - SETUP_BTN_W)
#define SETUP_MID_X  (SETUP_X + (SETUP_W - SETUP_BTN_W) / 2)
#define SETUP_TOP_Y  16                                    /* the on-tank pages: the title's band */
#define SETUP_TOP_BTN_W 96                                 /* name / look: the top row */
#define SETUP_TOP_BTN_Y (SETUP_TOP_Y + 24)
#define SETUP_TOP_BACK_X 32
#define SETUP_TOP_NEXT_X (TANK_W - 32 - SETUP_TOP_BTN_W)
/* the PAGES keyboard's top row sits INSIDE its panel, as it did on the 1.8
 * (its SETUP_TOP_* were panel-relative then; the square tank's are the open
 * tank's, which would put the row across the panel's top edge) */
#define SETUP_PANEL_TOP_BTN_Y  (SETUP_Y + 24)
#define SETUP_PANEL_TOP_BACK_X (SETUP_X + 12)
#define SETUP_PANEL_TOP_NEXT_X (SETUP_X + SETUP_W - 12 - SETUP_TOP_BTN_W)
/* the letter wheel: FISH_NAME_MAX slots of a 6x font (30 x 42 px glyphs) at
 * a 44 px pitch across the middle of the tank, chevrons 40 px above and
 * below the active one; a press in the row band picks the nearest slot, the
 * bands above / below it are the chevrons' */
#define SETUP_SLOT_SCALE 6
#define SETUP_SLOT_PX  44
#define SETUP_SLOT_W   (5 * SETUP_SLOT_SCALE)
#define SETUP_SLOT_H   (7 * SETUP_SLOT_SCALE)
#define SETUP_SLOT_X   ((TANK_W - ((FISH_NAME_MAX - 1) * SETUP_SLOT_PX + SETUP_SLOT_W)) / 2)
#define SETUP_SLOT_Y   200
#define SETUP_ARROW_GAP 40
#define SETUP_SPIN_PX   30                                 /* drag travel per letter */
/* the colour page (third design too): no panel, the live FRY wears the pick
 * with a ring round it; one row of 8 body swatches, 42 x 60 px at a 46 px
 * pitch (46 x 64 at 50 on the square tank), a tall band around it so a low-landing finger still hits; the
 * accent is a "?" - a fry's markings come in as it grows */
#define SETUP_SW_N  LOOK_N
#define SETUP_SW_PX 50
#define SETUP_SW_W  46
#define SETUP_SW_H  64
#define SETUP_SW_X  ((TANK_W - (SETUP_SW_N - 1) * SETUP_SW_PX - SETUP_SW_W) / 2)
#define SETUP_SW_Y  230
#define SETUP_ACC_Y 340
/* the family page (birth flow): the portrait ringed under the name, then
 * four rows - BODY / MARKINGS (a swatch, whose it is), BOLD / SOCIAL (a bar
 * of the fry's own, the parents' ticks in their body colours) - and a
 * PARENTS legend naming both in their tick colours; BACK + DONE at the foot */
#define SETUP_FAM_PORTRAIT_Y (SETUP_Y + 104)
#define SETUP_FAM_ROW_Y      (SETUP_Y + 168)
#define SETUP_FAM_ROW_DY     32
#define SETUP_FAM_LABEL_X    (SETUP_X + 28)
#define SETUP_FAM_VALUE_X    (SETUP_X + 150)
#define SETUP_FAM_BAR_W      210
#define SETUP_FAM_BAR_H      8
/* the placement page's DEPTH control (second design, 2026-09-16 - Strato:
 * "initially i thought 'back' button referred to menu navigation .. the
 * z-layer targeting and buttons are a bit ambiguous"): ONE outlined bar of
 * three joined segments under a DEPTH caption - the settings page's idiom,
 * a setting with three values, not three buttons - each segment a picture
 * tile (two small leaves and the keeper's first fish, drawn in that order:
 * the fish over the leaves, woven between them, behind them) with BEHIND /
 * AMONG / IN FRONT under it; a hint line under the bar says what to watch
 * for ("THE FISH SWIM THROUGH IT"). DONE stays alone top right in the go
 * teal, the bar in the calm ink, so "choose" and "finish" read apart. The
 * water below SETUP_PLACE_Y is the drag zone. */
#define SETUP_DEPTH_SEG_W  120
#define SETUP_DEPTH_W      (DECOR_Z_N * SETUP_DEPTH_SEG_W)      /* the plant's three segments; the castle's two (BEHIND / IN
                                                             * FRONT, Strato: no AMONG) are centred the same way - setup.c
                                                             * sizes the bar from tank_decor_z_count */
#define SETUP_DEPTH_X      ((TANK_W - SETUP_DEPTH_W) / 2)
#define SETUP_DEPTH_Y      (SETUP_TOP_BTN_Y + SETUP_BTN_H + 24)
#define SETUP_DEPTH_H      70
#define SETUP_DEPTH_TILE_H 40
#define SETUP_DEPTH_HINT_Y (SETUP_DEPTH_Y + SETUP_DEPTH_H + 8)
#define SETUP_PLACE_Y      (SETUP_DEPTH_HINT_Y + 20)
/* the stage: the clear spot each page leaves for the fish being edited
 * (tank_t.stage_*), top centre between the buttons */
#define SETUP_STAGE_X   (TANK_W / 2)
#define SETUP_STAGE_NAME_Y 116
#define SETUP_STAGE_LOOK_Y 150
/* the on-tank pages' foot captions ("DRAG THEM LEFT OR RIGHT") and the
   birth announcement's band */
#define SETUP_DRAG_CAPTION_Y 390
#define SETUP_BORN_Y         200
#endif
