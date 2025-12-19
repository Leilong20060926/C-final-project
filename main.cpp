// main.cpp
// Big-Two inspired (basic+UI) using raylib
// - Uses 4 suit textures (assets/spade.png, heart.png, club.png, diamond.png) that contain only suit artwork.
// - Card faces are drawn as rectangles; suit texture is drawn centered on each card.
// - Multi-select (1,2,5) + evaluate + scoring + basic Level & Magic choices.
// - Deck allocated with new/delete; shuffle via rand().
// Compile:
// g++ main.cpp -o main.exe -Iinclude -Llib -lraylib -lopengl32 -lgdi32 -lwinmm

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <vector>

extern "C" {
#include "raylib.h"
}

using namespace std;

enum Suit { DIAMOND=0, CLUB=1, HEART=2, SPADE=3 };

struct Card {
    int rank; // 2..14
    Suit suit;
};

static const array<string,4> SUIT_LET = {"D","C","H","S"};
static const array<string,13> RANK_STR = {"2","3","4","5","6","7","8","9","10","J","Q","K","A"};

// deck helpers using std::vector (safer than raw new/delete)
static vector<Card> make_deck() {
    vector<Card> deck; deck.reserve(52);
    for (int s = 0; s < 4; ++s) for (int r = 2; r <= 14; ++r) deck.push_back(Card{r, (Suit)s});
    return deck;
}

static void shuffle_deck(vector<Card> &deck) {
    static mt19937 rng((unsigned)time(nullptr));
    shuffle(deck.begin(), deck.end(), rng);
}

// Pop one card from top (back). Returns false if empty.
static bool deal_one(vector<Card> &deck, Card &out) {
    if (deck.empty()) return false;
    out = deck.back(); deck.pop_back(); return true;
}

// string for display
string card_top_text(const Card &c) {
    return RANK_STR[c.rank - 2];
}
string card_small_text(const Card &c) {
    return "(" + RANK_STR[c.rank - 2] + SUIT_LET[(int)c.suit] + ")";
}

// ---------- hand evaluation (1,2,5 support) ----------
void count_ranks_suits(const vector<Card> &hand, int rankCount[15], int suitCount[4]) {
    fill(rankCount, rankCount+15, 0);
    fill(suitCount, suitCount+4, 0);
    for (auto &c : hand) { rankCount[c.rank]++; suitCount[(int)c.suit]++; }
}

bool is_pair(const vector<Card>& hand) { return hand.size()==2 && hand[0].rank==hand[1].rank; }

bool is_straight(vector<Card> hand) {
    if (hand.size()!=5) return false;
    vector<int> r;
    for (auto &c : hand) r.push_back(c.rank);
    sort(r.begin(), r.end());
    // simple Ace-high only
    for (int i=1;i<5;++i) if (r[i] != r[i-1]+1) return false;
    // check duplicates
    for (int i=1;i<5;++i) if (r[i]==r[i-1]) return false;
    return true;
}
bool is_flush(const vector<Card>& hand) {
    if (hand.size()!=5) return false;
    for (size_t i=1;i<hand.size();++i) if (hand[i].suit != hand[0].suit) return false;
    return true;
}
bool is_full_house(const vector<Card>& hand) {
    if (hand.size()!=5) return false;
    int rc[15], sc[4]; count_ranks_suits(hand, rc, sc);
    bool has3=false, has2=false;
    for (int r=2;r<=14;++r) { if (rc[r]==3) has3=true; if (rc[r]==2) has2=true; }
    return has3 && has2;
}
bool is_four_of_a_kind(const vector<Card>& hand) {
    if (hand.size()!=5) return false;
    int rc[15], sc[4]; count_ranks_suits(hand, rc, sc);
    for (int r=2;r<=14;++r) if (rc[r]==4) return true;
    return false;
}
bool is_straight_flush(const vector<Card>& hand) {
    return is_straight(hand) && is_flush(hand);
}

enum EvalType { E_INVALID=0, E_SINGLE=1, E_PAIR=2, E_STRAIGHT=3, E_FLUSH=4, E_FULLHOUSE=5, E_FOUR=6, E_SFLUSH=7 };
EvalType evaluate_hand(const vector<Card> &hand) {
    if (hand.empty()) return E_INVALID;
    if (hand.size()==1) return E_SINGLE;
    if (hand.size()==2 && is_pair(hand)) return E_PAIR;
    if (hand.size()==5) {
        if (is_straight_flush(hand)) return E_SFLUSH;
        if (is_four_of_a_kind(hand)) return E_FOUR;
        if (is_full_house(hand)) return E_FULLHOUSE;
        if (is_flush(hand)) return E_FLUSH;
        if (is_straight(hand)) return E_STRAIGHT;
    }
    return E_INVALID;
}
int base_points_for_eval(EvalType t) {
    switch(t) {
        case E_SINGLE: return 1;
        case E_PAIR: return 2;
        case E_STRAIGHT: return 5;
        case E_FLUSH: return 6;
        case E_FULLHOUSE: return 8;
        case E_FOUR: return 10;
        case E_SFLUSH: return 12;
        default: return 0;
    }
}

// ---------- scoring & Magic (basic) ----------
struct MagicEffects {
    int add_pair = 0;
    int add_single = 0;
    int add_straight = 0;
    int add_flush = 0;
    int add_full = 0;
    int add_four = 0;
    int add_sflush = 0;
    // new effects
    bool drawBoostAvailable = false; // one-time per level ability: extra cards drawn after plays
    bool discardRedrawAvailable = false; // one-time per level ability
    int cardMultiplierRank = 0; // rank to multiply (2..14), 0 = none
    int cardMultiplierFactor = 1; // multiplier factor (e.g., 2 = double)
};

// Chain multiplier & Gold system
struct ChainState {
    EvalType lastHandType = E_INVALID;
    int chainCount = 0; // how many consecutive hands in chain
    double chainMultiplier = 1.0;
    
    // valid chain sequences: single->pair->straight, pair->flush, straight->fullhouse, etc.
    bool isValidChain(EvalType current) const {
        if (lastHandType == E_INVALID) return false;
        // Define valid progressions:
        // E_SINGLE -> E_PAIR (1->2)
        // E_PAIR -> E_STRAIGHT (2->3)
        // E_STRAIGHT -> E_FLUSH (3->4)
        // E_FLUSH -> E_FULLHOUSE (4->5)
        // E_FULLHOUSE -> E_FOUR (5->6)
        // E_FOUR -> E_SFLUSH (6->7)
        // Also allow repeating same hand type
        if (current == lastHandType) return true;
        
        if (lastHandType == E_SINGLE && current == E_PAIR) return true;
        if (lastHandType == E_PAIR && (current == E_STRAIGHT || current == E_PAIR)) return true;
        if (lastHandType == E_STRAIGHT && (current == E_FLUSH || current == E_STRAIGHT)) return true;
        if (lastHandType == E_FLUSH && (current == E_FULLHOUSE || current == E_FLUSH)) return true;
        if (lastHandType == E_FULLHOUSE && (current == E_FOUR || current == E_FULLHOUSE)) return true;
        if (lastHandType == E_FOUR && (current == E_SFLUSH || current == E_FOUR)) return true;
        return false;
    }
    
    void updateChain(EvalType current) {
        if (isValidChain(current)) {
            chainCount++;
            chainMultiplier = 1.0 + (chainCount * 0.25); // 1.0 (first), 1.25, 1.5, 1.75, 2.0, etc.
        } else {
            chainCount = 1;
            chainMultiplier = 1.0;
        }
        lastHandType = current;
    }
    
    void reset() {
        lastHandType = E_INVALID;
        chainCount = 0;
        chainMultiplier = 1.0;
    }
};

double get_points(EvalType et, int level, const MagicEffects &m) {
    double base = (double)base_points_for_eval(et);
    if (level==2) {
        if (et==E_SINGLE) base = 0.5;
        if (et==E_PAIR) base = 4;
    } else if (level==3) {
        if (et==E_SINGLE) base = 0;
    }
    int extra=0;
    switch(et) {
        case E_SINGLE: extra = m.add_single; break;
        case E_PAIR: extra = m.add_pair; break;
        case E_STRAIGHT: extra = m.add_straight; break;
        case E_FLUSH: extra = m.add_flush; break;
        case E_FULLHOUSE: extra = m.add_full; break;
        case E_FOUR: extra = m.add_four; break;
        case E_SFLUSH: extra = m.add_sflush; break;
        default: extra = 0;
    }
    // return fractional points (preserve 0.5 for level 2 singles)
    return base + extra;
}

// ---------- UI helpers ----------
Rectangle cardRectAt(int idx, int total, int screenW, int cardW, int cardH, int bottomY) {
    int spacing = 18;
    int totalW = total * (cardW + spacing) - spacing;
    int startX = (screenW - totalW) / 2;
    int x = startX + idx * (cardW + spacing);
    return Rectangle{ (float)x, (float)bottomY, (float)cardW, (float)cardH };
}

int main() {
    srand((unsigned)time(nullptr));

    // window
    const int screenW = 1200, screenH = 760;
    InitWindow(screenW, screenH, "Big-Two (Suit-textures) - Basic Version");
    SetTargetFPS(60);
    
    // Initialize audio device
    InitAudioDevice();

    // card display sizes
    const int cardW = 90, cardH = 126; // choose appropriate on-screen size
    const float suitScale = 0.6f; // suit texture scale inside card
    const int cardTextSize = 22; // font size for rank text (top-left and bottom-right)
    const int infoTextSize = 28; // font size for the top-left status line

    // load suit textures (these textures are just the suit artwork)
    Texture2D tex_spade = {0}, tex_heart = {0}, tex_club = {0}, tex_diamond = {0};
    bool has_spade = false, has_heart=false, has_club=false, has_diamond=false;
    if (FileExists("assets/spade.png")) { 
        tex_spade = LoadTexture("assets/spade.png"); 
        has_spade = tex_spade.id!=0; 
    }
    if (FileExists("assets/heart.png")) { 
        tex_heart = LoadTexture("assets/heart.png"); 
        has_heart = tex_heart.id!=0; 
    }
    if (FileExists("assets/club.png")) { 
        tex_club = LoadTexture("assets/club.png"); 
        has_club = tex_club.id!=0; 
    }
    if (FileExists("assets/diamond.png")) { 
        tex_diamond = LoadTexture("assets/diamond.png"); 
        has_diamond = tex_diamond.id!=0; 
    }
    // load magic card textures (optional)
    Texture2D tex_magicSuitChange = {0}, tex_magicHandScore = {0};
    Texture2D tex_magicCardMult = {0}, tex_magicDiscardRedraw = {0}, tex_magicDrawBoost = {0};
    bool has_magicSuitChange = false, has_magicHandScore = false;
    bool has_magicCardMult = false, has_magicDiscardRedraw = false, has_magicDrawBoost = false;
    if (FileExists("assets/SuitChange.png")) { 
        tex_magicSuitChange = LoadTexture("assets/SuitChange.png"); 
        has_magicSuitChange = tex_magicSuitChange.id!=0; 
    }
    if (FileExists("assets/HandScoreUpgrade.png")) { 
        tex_magicHandScore = LoadTexture("assets/HandScoreUpgrade.png"); 
        has_magicHandScore = tex_magicHandScore.id!=0; 
    }
    if (FileExists("assets/CardMultiplier.png")) { 
        tex_magicCardMult = LoadTexture("assets/CardMultiplier.png"); 
        has_magicCardMult = tex_magicCardMult.id!=0; 
    }
    if (FileExists("assets/DiscardRedraw.png")) { 
        tex_magicDiscardRedraw = LoadTexture("assets/DiscardRedraw.png"); 
        has_magicDiscardRedraw = tex_magicDiscardRedraw.id!=0; 
    }
    if (FileExists("assets/DrawBoost.png")) { 
        tex_magicDrawBoost = LoadTexture("assets/DrawBoost.png"); 
        has_magicDrawBoost = tex_magicDrawBoost.id!=0; 
    }
    
    // load audio sounds
    Sound snd_start = {0};
    bool has_start = false;
    if (FileExists("assets/start.mp3")) { 
        snd_start = LoadSound("assets/start.mp3"); 
        has_start = snd_start.frameCount > 0; 
    }
    
    Sound snd_buyCards = {0};
    bool has_buyCards = false;
    if (FileExists("assets/buyCards.mp3")) { 
        snd_buyCards = LoadSound("assets/buyCards.mp3"); 
        has_buyCards = snd_buyCards.frameCount > 0; 
    }
    
    Sound snd_nextLevel = {0};
    bool has_nextLevel = false;
    if (FileExists("assets/nextLevel.mp3")) { 
        snd_nextLevel = LoadSound("assets/nextLevel.mp3"); 
        has_nextLevel = snd_nextLevel.frameCount > 0; 
    }
    
    Sound snd_gameOver = {0};
    bool has_gameOver = false;
    if (FileExists("assets/gameOver.mp3")) { 
        snd_gameOver = LoadSound("assets/gameOver.mp3"); 
        has_gameOver = snd_gameOver.frameCount > 0; 
    }
    
    Sound snd_success = {0};
    bool has_success = false;
    if (FileExists("assets/scccess.mp3")) { 
        snd_success = LoadSound("assets/scccess.mp3"); 
        has_success = snd_success.frameCount > 0; 
    }

    // deck (vector)
    vector<Card> deck = make_deck();
    shuffle_deck(deck);

    // player hand (stack-managed vector)
    vector<Card> hand;
    for (int i = 0; i < 7 && !deck.empty(); ++i) { Card c; deal_one(deck, c); hand.push_back(c); }

    // discard pile (for Discard/Redraw magic)
    vector<Card> discardPile;

    // game state
    int level = 1;
    const double levelTarget[4] = {0.0, 55.0, 60.0, 65.0};
    double score = 0.0;
    double gold = 0.0; // currency earned from scoring
    bool levelCleared = false;
    bool gameFailed = false;
    bool finishedAll = false;
    MagicEffects magic; // persistent
    bool failBlockedByDiscardLogged = false;
    ChainState chain; // tracking for chain multiplier system
    bool showingShop = false; // whether shop UI is active
    bool soundGameOverPlayed = false; // track if end game sound played
    bool soundLevelUpPlayed = false; // track if level up sound played

    vector<int> selected; // indices
    vector<string> logs;
    bool justUsedRedraw = false; // skip fail-check immediately after using REDRAW
    auto pushLog = [&](const string &s){ logs.push_back(s); if (logs.size()>8) logs.erase(logs.begin()); };

    pushLog("Welcome. Multi-select cards then PLAY. Use PASS to skip.");
    
    // Play start sound
    if (has_start) PlaySound(snd_start);

    Rectangle playBtn{ screenW-220, screenH-120, 90, 40 };
    Rectangle passBtn{ screenW-120, screenH-120, 90, 40 };
    Rectangle redrawBtn{ screenW-220, screenH-170, 90, 40 };

    // main loop
    while (!WindowShouldClose()) {
        // input handling: clicks
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !levelCleared && !gameFailed && !finishedAll) {
            Vector2 m = GetMousePosition();
            // check cards
            int total = (int)hand.size();
            int bottomY = screenH - cardH - 40;
            for (int i=0;i<total;++i) {
                Rectangle r = cardRectAt(i, total, screenW, cardW, cardH, bottomY);
                if (m.x >= r.x && m.x <= r.x + r.width && m.y >= r.y && m.y <= r.y + r.height) {
                    auto it = find(selected.begin(), selected.end(), i);
                    if (it != selected.end()) selected.erase(it);
                    else { selected.push_back(i); sort(selected.begin(), selected.end()); }
                }
            }
            // play button
            if (m.x >= playBtn.x && m.x <= playBtn.x + playBtn.width && m.y >= playBtn.y && m.y <= playBtn.y + playBtn.height) {
                // collect selected cards into vector
                vector<Card> played;
                for (int idx : selected) {
                    if (idx>=0 && idx < (int)hand.size()) played.push_back(hand[idx]);
                }
                int cardsPlayed = (int)played.size();
                pushLog(TextFormat("PLAY: cardsPlayed=%d", cardsPlayed));
                EvalType et = evaluate_hand(played);
                if (et == E_INVALID) {
                    pushLog("Invalid play. Try 1, 2 (pair), or 5-card combos.");
                } else {
                    double pts = get_points(et, level, magic);
                    
                    // apply chain multiplier if valid sequence
                    double chainMult = 1.0;
                    if (chain.isValidChain(et)) {
                        chain.updateChain(et);
                        chainMult = chain.chainMultiplier;
                        pushLog(TextFormat("CHAIN x%.2f! (chain %d)", chainMult, chain.chainCount));
                    } else {
                        chain.updateChain(et);
                        chainMult = 1.0;
                    }
                    pts *= chainMult;
                    
                    // apply card multiplier if any of the played cards match the multiplier rank
                    if (magic.cardMultiplierRank >= 2 && magic.cardMultiplierFactor > 1) {
                        for (auto &pc : played) {
                            if (pc.rank == magic.cardMultiplierRank) { pts *= magic.cardMultiplierFactor; break; }
                        }
                    }
                    score += pts;
                    double goldEarned = pts * 0.5; // earn 0.5 gold per point
                    gold += goldEarned;
                    pushLog(TextFormat("Played %d cards => type %d, +%.1f pts, +%.0f gold", (int)played.size(), (int)et, pts, goldEarned));
                    // remove from hand from end->begin and add to discard pile
                    for (int k=(int)selected.size()-1;k>=0;--k) {
                        int idx = selected[k];
                        if (idx>=0 && idx < (int)hand.size()) {
                            discardPile.push_back(hand[idx]);
                            hand.erase(hand.begin()+idx);
                        }
                    }
                    selected.clear();
                    // first: fill up to 7 base hand size (from deck only)
                    while ((int)hand.size() < 7 && !deck.empty()) {
                        Card c; deal_one(deck, c); hand.push_back(c);
                    }
                    // second: if drawBoostAvailable, draw cardsPlayed additional random cards from discard pile (one-time use)
                    int randomDrawn = 0;
                    if (magic.drawBoostAvailable) {
                        pushLog(TextFormat("Draw Boost: drawing %d cards from discard pile.", cardsPlayed));
                        for (int i = 0; i < cardsPlayed; ++i) {
                            if (!discardPile.empty()) {
                                int randIdx = rand() % discardPile.size();
                                hand.push_back(discardPile[randIdx]);
                                discardPile.erase(discardPile.begin() + randIdx);
                                ++randomDrawn;
                            }
                        }
                        pushLog(TextFormat("Draw Boost: drew %d random cards from discard (used up).", randomDrawn));
                        magic.drawBoostAvailable = false; // one-time use
                    }
                }
            }
            // pass
            if (m.x >= passBtn.x && m.x <= passBtn.x + passBtn.width && m.y >= passBtn.y && m.y <= passBtn.y + passBtn.height) {
                pushLog("Passed. Drawing up to 7.");
                selected.clear();
                // fill up to 7 base hand size (from deck only)
                while ((int)hand.size() < 7 && !deck.empty()) {
                    Card c; deal_one(deck, c); hand.push_back(c);
                }
                // PASS doesn't draw extra random cards (cardsPlayed = 0)
            }

            // redraw (discard & redraw) - only if available
            if (m.x >= redrawBtn.x && m.x <= redrawBtn.x + redrawBtn.width && m.y >= redrawBtn.y && m.y <= redrawBtn.y + redrawBtn.height) {
                if (magic.discardRedrawAvailable) {
                    int handSize = (int)hand.size();
                    pushLog(TextFormat("Used Discard/Redraw: drawing %d random cards from played cards first.", handSize));
                    // first: randomly draw handSize cards from discard pile into a temp vector
                    vector<Card> newCards;
                    for (int i=0; i<handSize && !discardPile.empty(); ++i) {
                        int randIdx = rand() % discardPile.size();
                        newCards.push_back(discardPile[randIdx]);
                        discardPile.erase(discardPile.begin() + randIdx);
                    }
                    // then: add current hand to discard pile
                    for (auto &c : hand) discardPile.push_back(c);
                    // finally: replace hand with new cards (from discard)
                    hand.clear();
                    for (auto &c : newCards) hand.push_back(c);
                    // if we didn't get any from discard, try drawing from deck as fallback
                    int drawnFromDeck = 0;
                    int handSizeNeeded = (int)newCards.size() == 0 ? handSize : 0; // only top-up if nothing drawn
                    while (handSizeNeeded > 0 && !deck.empty()) {
                        Card c; deal_one(deck, c); hand.push_back(c); --handSizeNeeded; ++drawnFromDeck;
                    }
                    pushLog(TextFormat("Hand now has %d cards (deck draws: %d).", (int)hand.size(), drawnFromDeck));
                    magic.discardRedrawAvailable = false;
                    // if after all attempts hand is still empty and no deck/discard left, it's game over
                    if (hand.empty() && deck.empty() && discardPile.empty()) {
                        gameFailed = true;
                        pushLog("No cards available after REDRAW -> failed.");
                    } else {
                        pushLog(TextFormat("REDRAW complete: hand=%d, deck=%d, discard=%d", (int)hand.size(), (int)deck.size(), (int)discardPile.size()));
                    }
                } else {
                    pushLog("No redraw available.");
                }
            }
        }

        // check level clear
        if (!levelCleared && score >= levelTarget[level]) {
            levelCleared = true;
            showingShop = true; // show shop before magic choice
            pushLog(TextFormat("Level %d cleared! Visit Shop. Gold:%.0f", level, gold));
        }
        // fail check: if deck empty and hand empty and score < target -> fail (ignore discard)
        if (!gameFailed && deck.empty() && hand.empty() && score < levelTarget[level]) {
            gameFailed = true;
            pushLog("Deck empty and hand empty -> GAME OVER.");
        }

        // show magic choice when level cleared
        static bool choosingMagic = false;
        if (levelCleared && !finishedAll && !showingShop) choosingMagic = true;
        
        // draw
        BeginDrawing();
        ClearBackground(DARKGRAY);

        // show finished or fail immediately - overlays everything
        if (gameFailed || finishedAll) {
            // Play end game sound (only once)
            if (!soundGameOverPlayed) {
                if (gameFailed && has_gameOver) PlaySound(snd_gameOver);
                else if (finishedAll && has_success) PlaySound(snd_success);
                soundGameOverPlayed = true;
            }
            
            DrawRectangle(0, 0, screenW, screenH, Color{0,0,0,150});
            string msg = gameFailed ? "You failed. Press R to restart." : "You cleared all levels! Press R to restart.";
            int msgW = MeasureText(msg.c_str(), 36);
            DrawText(msg.c_str(), screenW/2 - msgW/2, screenH/2 - 50, 36, YELLOW);
            if (IsKeyPressed(KEY_R)) {
                deck = make_deck(); shuffle_deck(deck);
                hand.clear();
                for (int i=0;i<7 && !deck.empty();++i) { Card c; deal_one(deck, c); hand.push_back(c); }
                level=1; score=0.0; gold=0.0; 
                levelCleared=false; gameFailed=false; finishedAll=false; 
                magic = MagicEffects(); chain.reset();
                selected.clear(); logs.clear(); discardPile.clear(); 
                showingShop = false; choosingMagic = false; soundGameOverPlayed = false; soundLevelUpPlayed = false;
                pushLog("Restarted.");
                if (has_start) PlaySound(snd_start);
            }
            EndDrawing();
            continue;
        }

           DrawText(TextFormat("Level %d  Target:%.0f  Score:%.1f  Gold:%.0f  Chain:%d(x%.2f)  Deck:%d  Hand:%d  Discard:%d", level, levelTarget[level], score, gold, chain.chainCount, chain.chainMultiplier, (int)deck.size(), (int)hand.size(), (int)discardPile.size()),
               20, 10, infoTextSize, RAYWHITE);
        // logs
        int infoX=20, infoY=50;
        DrawRectangleLines(infoX-6, infoY-6, 420, 200, BLACK);
        DrawText("Log:", infoX, infoY, 18, RAYWHITE);
        for (size_t i=0;i<logs.size();++i) DrawText(logs[i].c_str(), infoX, infoY + 24 + (int)i*20, 16, RAYWHITE);

        // draw player's hand
        int total = (int)hand.size();
        int bottomY = screenH - cardH - 40;
        for (int i=0;i<total;++i) {
            Rectangle r = cardRectAt(i, total, screenW, cardW, cardH, bottomY);
            bool sel = (find(selected.begin(), selected.end(), i) != selected.end());
            Rectangle drawR = r;
            if (sel) drawR.y -= 16;
            // card background
            DrawRectangleRec(drawR, sel ? SKYBLUE : RAYWHITE);
            DrawRectangleLinesEx(drawR, 2, BLACK);
            // draw suit texture centered inside card
            Texture2D *tex = nullptr;
            switch (hand[i].suit) {
                case SPADE: if (has_spade) tex = &tex_spade; break;
                case HEART: if (has_heart) tex = &tex_heart; break;
                case CLUB: if (has_club) tex = &tex_club; break;
                case DIAMOND: if (has_diamond) tex = &tex_diamond; break;
            }
            if (tex) {
                // scale suit texture to fit
                float tw = (float)tex->width;
                float th = (float)tex->height;
                float scale = suitScale * (float)cardW / tw * 0.8f; // adjust
                Rectangle src{0,0, tw, th};
                Rectangle dest{ drawR.x + drawR.width/2 - (tw*scale)/2, drawR.y + drawR.height/2 - (th*scale)/2,
                                tw*scale, th*scale };
                DrawTexturePro(*tex, src, dest, Vector2{0,0}, 0.0f, RAYWHITE);
            } else {
                // fallback: draw a small circle to indicate suit
                Vector2 center{ drawR.x + drawR.width/2, drawR.y + drawR.height/2 };
                DrawCircleV(center, 14, LIGHTGRAY);
            }
            // draw top text (rank only)
            string top = card_top_text(hand[i]);
            Color topc = ( hand[i].suit == HEART || hand[i].suit == DIAMOND ) ? RED : BLACK;
            DrawText(top.c_str(), (int)drawR.x + 6, (int)drawR.y + 6, cardTextSize, topc);
            // draw bottom-right text (same rank as top)
            string bottom = card_top_text(hand[i]);
            int textW = MeasureText(bottom.c_str(), cardTextSize);
            DrawText(bottom.c_str(), (int)(drawR.x + drawR.width - textW - 6), (int)(drawR.y + drawR.height - (cardTextSize + 6)), cardTextSize, topc);
        }

        // draw buttons
        DrawRectangleRec(playBtn, BLUE); DrawText("PLAY", (int)playBtn.x+22, (int)playBtn.y+8, 20, WHITE);
        DrawRectangleRec(passBtn, RED); DrawText("PASS", (int)passBtn.x+18, (int)passBtn.y+8, 20, WHITE);
        // draw redraw button only if available
        if (magic.discardRedrawAvailable) {
            DrawRectangleRec(redrawBtn, PURPLE); DrawText("REDRAW", (int)redrawBtn.x+8, (int)redrawBtn.y+8, 18, WHITE);
        }

        // if choosing magic show options and handle clicks
        if (choosingMagic && !gameFailed && !finishedAll) {
            // layout five options centered
            const int optW = 160, optH = 140, spacing = 20;
            int startX = screenW/2 - (5*optW + 4*spacing)/2;
            Rectangle opt1{(float)startX, (float)(screenH/2 - 80), (float)optW, (float)optH};
            Rectangle opt2{(float)(startX + (optW+spacing)), (float)(screenH/2 - 80), (float)optW, (float)optH};
            Rectangle opt3{(float)(startX + 2*(optW+spacing)), (float)(screenH/2 - 80), (float)optW, (float)optH};
            Rectangle opt4{(float)(startX + 3*(optW+spacing)), (float)(screenH/2 - 80), (float)optW, (float)optH};
            Rectangle opt5{(float)(startX + 4*(optW+spacing)), (float)(screenH/2 - 80), (float)optW, (float)optH};
            // draw all five option boxes with same background and border
            const Color optBg = LIGHTGRAY;
            DrawRectangleRec(opt1, optBg); DrawRectangleRec(opt2, optBg); DrawRectangleRec(opt3, optBg); DrawRectangleRec(opt4, optBg); DrawRectangleRec(opt5, optBg);
            DrawRectangleLinesEx(opt1, 2, BLACK); DrawRectangleLinesEx(opt2, 2, BLACK); DrawRectangleLinesEx(opt3, 2, BLACK); DrawRectangleLinesEx(opt4, 2, BLACK); DrawRectangleLinesEx(opt5, 2, BLACK);
            // titles
            const char *titleA = "Hand Score Upgrade"; // opt1
            const char *titleB = "Suit Change";        // opt2
            const char *titleC = "Card Multiplier";   // opt3
            const char *titleD = "Discard / Redraw";  // opt4
            const char *titleE = "Draw Boost";        // opt5
            int titleSize = 16;
            DrawText(titleA, (int)(opt1.x + (opt1.width - MeasureText(titleA, titleSize))/2), (int)(opt1.y + 6), titleSize, BLACK);
            DrawText(titleB, (int)(opt2.x + (opt2.width - MeasureText(titleB, titleSize))/2), (int)(opt2.y + 6), titleSize, BLACK);
            DrawText(titleC, (int)(opt3.x + (opt3.width - MeasureText(titleC, titleSize))/2), (int)(opt3.y + 6), titleSize, BLACK);
            DrawText(titleD, (int)(opt4.x + (opt4.width - MeasureText(titleD, titleSize))/2), (int)(opt4.y + 6), titleSize, BLACK);
            DrawText(titleE, (int)(opt5.x + (opt5.width - MeasureText(titleE, titleSize))/2), (int)(opt5.y + 6), titleSize, BLACK);
            // images drawn below titles (if available), otherwise fallback text per option
            // opt1: HandScoreUpgrade
            if (has_magicHandScore) {
                Rectangle src{0,0,(float)tex_magicHandScore.width,(float)tex_magicHandScore.height};
                Rectangle dest{ opt1.x + 10, opt1.y + 26, opt1.width - 20, opt1.height - 40 };
                DrawTexturePro(tex_magicHandScore, src, dest, Vector2{0,0}, 0.0f, WHITE);
            } else DrawText("+3 to Pairs (perm)", (int)opt1.x+10, (int)opt1.y+30, 14, BLACK);
            // opt2: SuitChange
            if (has_magicSuitChange) {
                Rectangle src2{0,0,(float)tex_magicSuitChange.width,(float)tex_magicSuitChange.height};
                Rectangle dest2{ opt2.x + 10, opt2.y + 26, opt2.width - 20, opt2.height - 40 };
                DrawTexturePro(tex_magicSuitChange, src2, dest2, Vector2{0,0}, 0.0f, WHITE);
            } else DrawText("Click to change suit", (int)opt2.x+10, (int)opt2.y+30, 14, BLACK);
            // opt3: Card Multiplier
            if (has_magicCardMult) {
                Rectangle src3{0,0,(float)tex_magicCardMult.width,(float)tex_magicCardMult.height};
                Rectangle dest3{ opt3.x + 10, opt3.y + 26, opt3.width - 20, opt3.height - 40 };
                DrawTexturePro(tex_magicCardMult, src3, dest3, Vector2{0,0}, 0.0f, WHITE);
            } else DrawText("Double points for a rank", (int)opt3.x+10, (int)opt3.y+30, 14, BLACK);
            // opt4: Discard/Redraw
            if (has_magicDiscardRedraw) {
                Rectangle src4{0,0,(float)tex_magicDiscardRedraw.width,(float)tex_magicDiscardRedraw.height};
                Rectangle dest4{ opt4.x + 10, opt4.y + 26, opt4.width - 20, opt4.height - 40 };
                DrawTexturePro(tex_magicDiscardRedraw, src4, dest4, Vector2{0,0}, 0.0f, WHITE);
            } else DrawText("Discard & redraw hand", (int)opt4.x+10, (int)opt4.y+30, 14, BLACK);
            // opt5: Draw Boost
            if (has_magicDrawBoost) {
                Rectangle src5{0,0,(float)tex_magicDrawBoost.width,(float)tex_magicDrawBoost.height};
                Rectangle dest5{ opt5.x + 10, opt5.y + 26, opt5.width - 20, opt5.height - 40 };
                DrawTexturePro(tex_magicDrawBoost, src5, dest5, Vector2{0,0}, 0.0f, WHITE);
            } else DrawText("+extra draws after play", (int)opt5.x+10, (int)opt5.y+30, 14, BLACK);
            DrawText("Click option to choose", screenW/2 - 80, screenH/2 + 100, 16, RAYWHITE);

            // click handling within choosing state (five options)
                if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                Vector2 m = GetMousePosition();
                auto finishChoose = [&](const string &logMsg){ 
                    pushLog(logMsg); 
                    choosingMagic=false; 
                    levelCleared=false; 
                    level++; 
                    if (level>3) { 
                        finishedAll=true; 
                        pushLog("All levels cleared!"); 
                    } else { 
                        if (has_nextLevel) PlaySound(snd_nextLevel);
                        pushLog(TextFormat("Starting Level %d (target %d)", level, levelTarget[level])); 
                    } 
                };
                // opt1: HandScoreUpgrade (+3 to pairs)
                if (m.x >= opt1.x && m.x <= opt1.x + opt1.width && m.y >= opt1.y && m.y <= opt1.y + opt1.height) {
                    magic.add_pair += 3; finishChoose("Chosen: Hand Score Upgrade (+3 to Pairs)");
                }
                // opt2: SuitChange (click a card to cycle suit)
                else if (m.x >= opt2.x && m.x <= opt2.x + opt2.width && m.y >= opt2.y && m.y <= opt2.y + opt2.height) {
                    pushLog("Chosen: Suit Change - click a card to change its suit.");
                    bool changed=false;
                    while (!changed && !WindowShouldClose()) {
                        BeginDrawing();
                        DrawText("Click a card to change its suit...", screenW/2 - 140, screenH/2 + 110, 18, YELLOW);
                        EndDrawing();
                        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                            Vector2 m2 = GetMousePosition();
                            int tot = (int)hand.size();
                            int bY = screenH - cardH - 40;
                            for (int i=0;i<tot;++i) {
                                Rectangle r = cardRectAt(i, tot, screenW, cardW, cardH, bY);
                                if (m2.x >= r.x && m2.x <= r.x + r.width && m2.y >= r.y && m2.y <= r.y + r.height) {
                                    hand[i].suit = static_cast<Suit>((((int)hand[i].suit)+1) % 4);
                                    pushLog(TextFormat("Changed suit of card to %s", card_top_text(hand[i]).c_str()));
                                    changed = true; break;
                                }
                            }
                        }
                    }
                    finishChoose("Completed Suit Change.");
                }
                // opt3: Card Multiplier (click a card to select rank to multiply)
                else if (m.x >= opt3.x && m.x <= opt3.x + opt3.width && m.y >= opt3.y && m.y <= opt3.y + opt3.height) {
                    pushLog("Chosen: Card Multiplier - click a card in your hand to select its rank for doubling.");
                    bool chosen=false;
                    while (!chosen && !WindowShouldClose()) {
                        BeginDrawing(); 
                        DrawText("Click a card to select rank for multiplier...", screenW/2 - 200, screenH/2 + 110, 18, YELLOW); EndDrawing();
                        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                            Vector2 m2 = GetMousePosition(); int tot=(int)hand.size(); int bY=screenH - cardH - 40;
                            for (int i=0;i<tot;++i) {
                                Rectangle r = cardRectAt(i, tot, screenW, cardW, cardH, bY);
                                if (m2.x >= r.x && m2.x <= r.x + r.width && m2.y >= r.y && m2.y <= r.y + r.height) {
                                    magic.cardMultiplierRank = hand[i].rank;
                                    magic.cardMultiplierFactor = 2; // double points
                                    pushLog(TextFormat("Card Multiplier set to rank %s (x%d)", RANK_STR[magic.cardMultiplierRank-2].c_str(), magic.cardMultiplierFactor));
                                    chosen=true; break;
                                }
                            }
                        }
                    }
                    finishChoose("Card Multiplier applied.");
                }
                // opt4: Discard / Redraw (one-time per level)
                else if (m.x >= opt4.x && m.x <= opt4.x + opt4.width && m.y >= opt4.y && m.y <= opt4.y + opt4.height) {
                    magic.discardRedrawAvailable = true; finishChoose("Chosen: Discard/Redraw (one-time this level)");
                }
                // opt5: Draw Boost (one-time: draw extra cards on next play)
                else if (m.x >= opt5.x && m.x <= opt5.x + opt5.width && m.y >= opt5.y && m.y <= opt5.y + opt5.height) {
                    magic.drawBoostAvailable = true; finishChoose("Chosen: Draw Boost (extra draw on next play only)");
                }
            }
        }

        // ===== SHOP UI (rendered on top at the end) =====
        if (showingShop && !gameFailed && !finishedAll) {
            const int shopW = 900, shopH = 500;
            int shopX = (screenW - shopW) / 2;
            int shopY = (screenH - shopH) / 2;
            
            // dark overlay
            DrawRectangle(0, 0, screenW, screenH, Color{0,0,0,100});
            
            // shop background
            DrawRectangleRounded(Rectangle{(float)shopX, (float)shopY, (float)shopW, (float)shopH}, 0.1f, 20, DARKGRAY);
            DrawRectangleLinesEx(Rectangle{(float)shopX, (float)shopY, (float)shopW, (float)shopH}, 4, WHITE);
            
            // title and gold display
            DrawText("SHOP - Upgrade Your Magic", shopX + 50, shopY + 20, 32, YELLOW);
            DrawText(TextFormat("Available Gold: %.0f", gold), shopX + 50, shopY + 60, 24, LIME);
            
            // shop items (magic upgrades)
            const int itemW = 180, itemH = 120, startX = shopX + 40, startY = shopY + 110;
            const int itemSpacing = 20;
            
            // Item 1: Extra Pair Bonus (cost: 30 gold)
            int item1X = startX, item1Y = startY;
            DrawRectangleRounded(Rectangle{(float)item1X, (float)item1Y, (float)itemW, (float)itemH}, 0.05f, 10, Color{173,216,230,255});
            DrawRectangleLinesEx(Rectangle{(float)item1X, (float)item1Y, (float)itemW, (float)itemH}, 2, BLACK);
            DrawText("Pair Bonus", item1X + 20, item1Y + 10, 16, BLACK);
            DrawText("+5 points/pair", item1X + 12, item1Y + 32, 14, BLACK);
            DrawText("Cost: 30 gold", item1X + 18, item1Y + 55, 12, RED);
            bool canBuy1 = (gold >= 30);
            DrawRectangle(item1X + 120, item1Y + 50, 45, 25, canBuy1 ? GREEN : DARKGRAY);
            DrawText(canBuy1 ? "BUY" : "---", item1X + 128, item1Y + 56, 14, BLACK);
            Rectangle rect1{(float)item1X, (float)item1Y, (float)itemW, (float)itemH};
            
            // Item 2: Straight Bonus (cost: 40 gold)
            int item2X = startX + itemW + itemSpacing, item2Y = startY;
            DrawRectangleRounded(Rectangle{(float)item2X, (float)item2Y, (float)itemW, (float)itemH}, 0.05f, 10, Color{144,238,144,255});
            DrawRectangleLinesEx(Rectangle{(float)item2X, (float)item2Y, (float)itemW, (float)itemH}, 2, BLACK);
            DrawText("Straight Bonus", item2X + 10, item2Y + 10, 16, BLACK);
            DrawText("+7 points/straight", item2X + 8, item2Y + 32, 14, BLACK);
            DrawText("Cost: 40 gold", item2X + 18, item2Y + 55, 12, RED);
            bool canBuy2 = (gold >= 40);
            DrawRectangle(item2X + 120, item2Y + 50, 45, 25, canBuy2 ? GREEN : DARKGRAY);
            DrawText(canBuy2 ? "BUY" : "---", item2X + 128, item2Y + 56, 14, BLACK);
            Rectangle rect2{(float)item2X, (float)item2Y, (float)itemW, (float)itemH};
            
            // Item 3: Flush Bonus (cost: 50 gold)
            int item3X = startX + 2*(itemW + itemSpacing), item3Y = startY;
            DrawRectangleRounded(Rectangle{(float)item3X, (float)item3Y, (float)itemW, (float)itemH}, 0.05f, 10, Color{255,255,200,255});
            DrawRectangleLinesEx(Rectangle{(float)item3X, (float)item3Y, (float)itemW, (float)itemH}, 2, BLACK);
            DrawText("Flush Bonus", item3X + 20, item3Y + 10, 16, BLACK);
            DrawText("+8 points/flush", item3X + 15, item3Y + 32, 14, BLACK);
            DrawText("Cost: 50 gold", item3X + 18, item3Y + 55, 12, RED);
            bool canBuy3 = (gold >= 50);
            DrawRectangle(item3X + 120, item3Y + 50, 45, 25, canBuy3 ? GREEN : DARKGRAY);
            DrawText(canBuy3 ? "BUY" : "---", item3X + 128, item3Y + 56, 14, BLACK);
            Rectangle rect3{(float)item3X, (float)item3Y, (float)itemW, (float)itemH};
            
            // Item 4: Full House Bonus (cost: 60 gold)
            int item4X = startX, item4Y = startY + itemH + itemSpacing + 30;
            DrawRectangleRounded(Rectangle{(float)item4X, (float)item4Y, (float)itemW, (float)itemH}, 0.05f, 10, Color{221,160,221,255});
            DrawRectangleLinesEx(Rectangle{(float)item4X, (float)item4Y, (float)itemW, (float)itemH}, 2, BLACK);
            DrawText("Full House Bonus", item4X + 8, item4Y + 10, 16, BLACK);
            DrawText("+10 points/full", item4X + 15, item4Y + 32, 14, BLACK);
            DrawText("Cost: 60 gold", item4X + 18, item4Y + 55, 12, RED);
            bool canBuy4 = (gold >= 60);
            DrawRectangle(item4X + 120, item4Y + 50, 45, 25, canBuy4 ? GREEN : DARKGRAY);
            DrawText(canBuy4 ? "BUY" : "---", item4X + 128, item4Y + 56, 14, BLACK);
            Rectangle rect4{(float)item4X, (float)item4Y, (float)itemW, (float)itemH};
            
            // Item 5: Four of a Kind Bonus (cost: 75 gold)
            int item5X = startX + itemW + itemSpacing, item5Y = startY + itemH + itemSpacing + 30;
            DrawRectangleRounded(Rectangle{(float)item5X, (float)item5Y, (float)itemW, (float)itemH}, 0.05f, 10, ORANGE);
            DrawRectangleLinesEx(Rectangle{(float)item5X, (float)item5Y, (float)itemW, (float)itemH}, 2, BLACK);
            DrawText("Four of a Kind Bonus", item5X + 5, item5Y + 10, 14, BLACK);
            DrawText("+12 points/four", item5X + 15, item5Y + 32, 14, BLACK);
            DrawText("Cost: 75 gold", item5X + 18, item5Y + 55, 12, RED);
            bool canBuy5 = (gold >= 75);
            DrawRectangle(item5X + 120, item5Y + 50, 45, 25, canBuy5 ? GREEN : DARKGRAY);
            DrawText(canBuy5 ? "BUY" : "---", item5X + 128, item5Y + 56, 14, BLACK);
            Rectangle rect5{(float)item5X, (float)item5Y, (float)itemW, (float)itemH};
            
            // Item 6: Straight Flush Bonus (cost: 100 gold)
            int item6X = startX + 2*(itemW + itemSpacing), item6Y = startY + itemH + itemSpacing + 30;
            DrawRectangleRounded(Rectangle{(float)item6X, (float)item6Y, (float)itemW, (float)itemH}, 0.05f, 10, PINK);
            DrawRectangleLinesEx(Rectangle{(float)item6X, (float)item6Y, (float)itemW, (float)itemH}, 2, BLACK);
            DrawText("Straight Flush Bonus", item6X + 5, item6Y + 10, 14, BLACK);
            DrawText("+15 points/sf", item6X + 15, item6Y + 32, 14, BLACK);
            DrawText("Cost: 100 gold", item6X + 15, item6Y + 55, 12, RED);
            bool canBuy6 = (gold >= 100);
            DrawRectangle(item6X + 120, item6Y + 50, 45, 25, canBuy6 ? GREEN : DARKGRAY);
            DrawText(canBuy6 ? "BUY" : "---", item6X + 128, item6Y + 56, 14, BLACK);
            Rectangle rect6{(float)item6X, (float)item6Y, (float)itemW, (float)itemH};
            
            // Done button
            Rectangle doneBtn{(float)(shopX + shopW - 120), (float)(shopY + shopH - 50), 100.0f, 40.0f};
            DrawRectangleRec(doneBtn, PURPLE);
            DrawRectangleLinesEx(doneBtn, 2, WHITE);
            DrawText("Continue", (int)(doneBtn.x + 12), (int)(doneBtn.y + 10), 18, WHITE);
            
            // Handle shop clicks
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                Vector2 m = GetMousePosition();
                
                // Item 1: Pair Bonus
                if (canBuy1 && m.x >= rect1.x && m.x <= rect1.x + rect1.width && m.y >= rect1.y && m.y <= rect1.y + rect1.height) {
                    gold -= 30; magic.add_pair += 5; pushLog("Bought: +5 Pair Bonus"); if (has_buyCards) PlaySound(snd_buyCards);
                }
                // Item 2: Straight Bonus
                else if (canBuy2 && m.x >= rect2.x && m.x <= rect2.x + rect2.width && m.y >= rect2.y && m.y <= rect2.y + rect2.height) {
                    gold -= 40; magic.add_straight += 7; pushLog("Bought: +7 Straight Bonus"); if (has_buyCards) PlaySound(snd_buyCards);
                }
                // Item 3: Flush Bonus
                else if (canBuy3 && m.x >= rect3.x && m.x <= rect3.x + rect3.width && m.y >= rect3.y && m.y <= rect3.y + rect3.height) {
                    gold -= 50; magic.add_flush += 8; pushLog("Bought: +8 Flush Bonus"); if (has_buyCards) PlaySound(snd_buyCards);
                }
                // Item 4: Full House Bonus
                else if (canBuy4 && m.x >= rect4.x && m.x <= rect4.x + rect4.width && m.y >= rect4.y && m.y <= rect4.y + rect4.height) {
                    gold -= 60; magic.add_full += 10; pushLog("Bought: +10 Full House Bonus"); if (has_buyCards) PlaySound(snd_buyCards);
                }
                // Item 5: Four of a Kind Bonus
                else if (canBuy5 && m.x >= rect5.x && m.x <= rect5.x + rect5.width && m.y >= rect5.y && m.y <= rect5.y + rect5.height) {
                    gold -= 75; magic.add_four += 12; pushLog("Bought: +12 Four of a Kind Bonus"); if (has_buyCards) PlaySound(snd_buyCards);
                }
                // Item 6: Straight Flush Bonus
                else if (canBuy6 && m.x >= rect6.x && m.x <= rect6.x + rect6.width && m.y >= rect6.y && m.y <= rect6.y + rect6.height) {
                    gold -= 100; magic.add_sflush += 15; pushLog("Bought: +15 Straight Flush Bonus"); if (has_buyCards) PlaySound(snd_buyCards);
                }
                // Continue button
                else if (m.x >= doneBtn.x && m.x <= doneBtn.x + doneBtn.width && m.y >= doneBtn.y && m.y <= doneBtn.y + doneBtn.height) {
                    showingShop = false;
                    pushLog("Shop closed. Now choose Magic upgrade.");
                }
            }
        }

        EndDrawing();
    }

    // cleanup
    if (has_spade) UnloadTexture(tex_spade);
    if (has_heart) UnloadTexture(tex_heart);
    if (has_club) UnloadTexture(tex_club);
    if (has_diamond) UnloadTexture(tex_diamond);
    if (has_magicSuitChange) UnloadTexture(tex_magicSuitChange);
    if (has_magicHandScore) UnloadTexture(tex_magicHandScore);
    if (has_magicCardMult) UnloadTexture(tex_magicCardMult);
    if (has_magicDiscardRedraw) UnloadTexture(tex_magicDiscardRedraw);
    if (has_magicDrawBoost) UnloadTexture(tex_magicDrawBoost);
    if (has_start) UnloadSound(snd_start);
    if (has_buyCards) UnloadSound(snd_buyCards);
    if (has_nextLevel) UnloadSound(snd_nextLevel);
    if (has_gameOver) UnloadSound(snd_gameOver);
    if (has_success) UnloadSound(snd_success);
    CloseAudioDevice();
    // vectors clean up automatically
    CloseWindow();
    return 0;
}
