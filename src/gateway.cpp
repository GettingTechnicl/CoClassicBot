#include "gateway.h"
#include "CItem.h"
#include <algorithm>
#include <climits>
#include <unordered_map>
#include <queue>
#include <unordered_set>

namespace
{
    constexpr uint32_t kConductressSilverCost = 100;
    constexpr uint32_t kGuildGatewaySilverCost = 1000;
    constexpr int kMinSameMapItemGateDistance = 160;

    Gateway WithSilverCost(Gateway gateway, uint32_t silverCost)
    {
        gateway.silverCost = silverCost;
        return gateway;
    }
}

// =====================================================================
// Hardcoded gateway table
// =====================================================================
static const std::unordered_map<OBJID, std::vector<Gateway>> s_gateways = {
    {
        MAP_MARKET, {
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_TWIN_CITY, {430, 380}, "Mark.Controller"},
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_DESERT_CITY, {493, 650}, "Mark.Controller"},
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_PHOENIX_CASTLE, {193, 266}, "Mark.Controller"},
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_APE_MOUNTAIN, {566, 565}, "Mark.Controller"},
            {GatewayType::Npc, MAP_MARKET, {215, 220}, MAP_BIRD_ISLAND, {717, 577}, "Mark.Controller"},
        }
    },
    {
        MAP_TWIN_CITY, {
            // PC
            WithSilverCost({GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {958, 555}, "Conductress"},
                           kConductressSilverCost),
            // DC
            WithSilverCost({
                               GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {69, 473}, "Conductress", 1,
                               {1}
                           }, kConductressSilverCost),
            // AC
            WithSilverCost({
                               GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {555, 957}, "Conductress", 1,
                               {2}
                           }, kConductressSilverCost),
            // BI
            WithSilverCost({
                               GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {232, 190}, "Conductress", 1,
                               {3}
                           }, kConductressSilverCost),
            // Mine
            WithSilverCost({
                               GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_TWIN_CITY, {53, 399}, "Conductress", 1,
                               {4}
                           }, kConductressSilverCost),
            // Market
            WithSilverCost({GatewayType::Npc, MAP_TWIN_CITY, {435, 440}, MAP_MARKET, {211, 196}, "Conductress", 1, {5}},
                           kConductressSilverCost),

            // GuildController
            WithSilverCost({GatewayType::Npc, MAP_TWIN_CITY, {350, 337}, MAP_GUILD, {351, 341}, "GuildController"},
                           kGuildGatewaySilverCost),


            // DC
            {GatewayType::Npc, MAP_TWIN_CITY, {60, 463}, MAP_DESERT_CITY, {971, 666}, "GeneralPeace"},

            // Portals
            {GatewayType::Portal, MAP_TWIN_CITY, {962, 557}, MAP_PHOENIX_CASTLE, {11, 376}},
            // Session 12: "021" was a C++ octal literal (=17 decimal), not 21 -- landed 4 tiles off.
            {GatewayType::Portal, MAP_TWIN_CITY, {556, 964}, MAP_APE_MOUNTAIN, {381, 21}},
            {GatewayType::Portal, MAP_TWIN_CITY, {224, 196}, MAP_BIRD_ISLAND, {1010, 710}},
            {GatewayType::Portal, MAP_TWIN_CITY, {45, 395}, MAP_MINE_CAVE, {160, 96}}
        }
    },
    {
        MAP_DESERT_CITY, {
            // TC
            WithSilverCost({GatewayType::Npc, MAP_DESERT_CITY, {478, 631}, MAP_DESERT_CITY, {971, 668}, "Conductress"},
                           kConductressSilverCost),
            // Mystic Castle
            WithSilverCost({
                               GatewayType::Npc, MAP_DESERT_CITY, {478, 631}, MAP_DESERT_CITY, {85, 323}, "Conductress",
                               1, {1}
                           }, kConductressSilverCost),
            // Market
            WithSilverCost({
                               GatewayType::Npc, MAP_DESERT_CITY, {478, 631}, MAP_MARKET, {211, 196}, "Conductress", 1,
                               {2}
                           }, kConductressSilverCost),
            // Portals
            {GatewayType::Portal, MAP_DESERT_CITY, {977, 668}, MAP_TWIN_CITY, {69, 473}},
            {GatewayType::Portal, MAP_DESERT_CITY, {77, 320}, MAP_MYSTIC_CASTLE, {312, 646}},
            // Map file portal #3 -> New Desert City (the Adventure Zone entrance).
            // LIVE-VALIDATED 2026-09-02, walked twice:
            //   [portal] 1000 (163,471) -> 1077 (756,873) via portal #3 @(163,471)
            {GatewayType::Portal, MAP_DESERT_CITY, {163, 471}, MAP_NEW_DESERT_CITY, {756, 873}},
        }
    },
    {
        MAP_MYSTIC_CASTLE, {
            {GatewayType::Portal, MAP_MYSTIC_CASTLE, {311, 650}, MAP_DESERT_CITY, {85, 323}},
        }
    },
    {
        MAP_PHOENIX_CASTLE, {
            // TC
            WithSilverCost({
                               GatewayType::Npc, MAP_PHOENIX_CASTLE, {228, 255}, MAP_PHOENIX_CASTLE, {11, 376},
                               "Conductress"
                           }, kConductressSilverCost),
            // Market
            WithSilverCost({
                               GatewayType::Npc, MAP_PHOENIX_CASTLE, {228, 255}, MAP_MARKET, {211, 196}, "Conductress",
                               1, {1}
                           }, kConductressSilverCost),
            // Portals
            {GatewayType::Portal, MAP_PHOENIX_CASTLE, {6, 376}, MAP_TWIN_CITY, {958, 555}},
            // Direct Phoenix -> Ape Mountain link, from the community world
            // map the user supplied 2026-09-02 (Forest 452,819 <-> Canyon
            // 922,558). Source tile is the map file's own portal #2
            // (448,822); landing approximated as the partner's portal tile,
            // same convention as the Ape City 2 entry — the bot re-reads its
            // real position after the teleport. LIVE-VALIDATED 2026-09-02:
            //   [portal] 1011 (448,822) -> 1020 (919,559) via portal #2 @(448,822)
            {GatewayType::Portal, MAP_PHOENIX_CASTLE, {448, 822}, MAP_APE_MOUNTAIN, {919, 559}},
            // Phoenix Castle 2 (1076, newwoods.DMap) — an exact copy of this map,
            // same relationship as Ape Mountain -> Ape City 2. Map file portal #4.
            // LIVE-VALIDATED 2026-09-02 by the user's walk, both directions:
            //   [portal] 1011 (855,483) -> 1076 (11,361) via portal #4 @(855,482)
            //   [portal] 1076 (2,360)   -> 1011 (850,485) via portal #0 @(2,360)
            {GatewayType::Portal, MAP_PHOENIX_CASTLE, {855, 482}, MAP_PHOENIX_CASTLE2, {11, 361}},
        }
    },
    {
        MAP_DREAMLAND, {
            // Back to Ape Mountain. Live-validated (see the Ape Mountain entry).
            {GatewayType::Portal, MAP_DREAMLAND, {531, 534}, MAP_APE_MOUNTAIN, {23, 383}},
        }
    },
    {
        MAP_PHOENIX_CASTLE2, {
            // Back to Phoenix Castle. Live-validated (see above).
            {GatewayType::Portal, MAP_PHOENIX_CASTLE2, {2, 360}, MAP_PHOENIX_CASTLE, {850, 485}},
        }
    },
    {
        MAP_APE_MOUNTAIN, {
            // TC
            WithSilverCost({GatewayType::Npc, MAP_APE_MOUNTAIN, {566, 622}, MAP_APE_MOUNTAIN, {381, 21}, "Conductress"},
                           kConductressSilverCost),
            // Market
            WithSilverCost({
                               GatewayType::Npc, MAP_APE_MOUNTAIN, {566, 622}, MAP_MARKET, {211, 196}, "Conductress", 1,
                               {1}
                           }, kConductressSilverCost),
            // Portals
            {GatewayType::Portal, MAP_APE_MOUNTAIN, {377, 9}, MAP_TWIN_CITY, {555, 957}},
            // Direct Ape Mountain -> Phoenix link. Map file portal #0 (926,557).
            // LIVE-VALIDATED 2026-09-02 by the user's own walk:
            //   [portal] 1020 (926,557) -> 1011 (453,819)
            {GatewayType::Portal, MAP_APE_MOUNTAIN, {926, 557}, MAP_PHOENIX_CASTLE, {453, 819}},
            // Dreamland (1012) off the west edge. Map file portal #4.
            // LIVE-VALIDATED 2026-09-02 by the user's walk, both directions:
            //   [portal] 1020 (12,377)   -> 1012 (532,529) via portal #4 @(11,377)
            //   [portal] 1012 (531,534)  -> 1020 (23,383)  via portal #0 @(531,534)
            {GatewayType::Portal, MAP_APE_MOUNTAIN, {11, 377}, MAP_DREAMLAND, {532, 529}},
            // Ape City 2 (1075) — hunting map. Landing approximated as the
            // return portal's own tile (bot re-reads real pos after teleport).
            {GatewayType::Portal, MAP_APE_MOUNTAIN, {610, 874}, MAP_APE_CITY2, {361, 6}},
        }
    },
    {
        MAP_APE_CITY2, {
            // Back to Ape Mountain.
            {GatewayType::Portal, MAP_APE_CITY2, {361, 6}, MAP_APE_MOUNTAIN, {610, 874}},
        }
    },
    {
        MAP_BIRD_ISLAND, {
            // TC
            WithSilverCost({GatewayType::Npc, MAP_BIRD_ISLAND, {789, 566}, MAP_BIRD_ISLAND, {1010, 710}, "Conductress"},
                           kConductressSilverCost),
            // Market
            WithSilverCost({
                               GatewayType::Npc, MAP_BIRD_ISLAND, {789, 566}, MAP_MARKET, {211, 196}, "Conductress", 1,
                               {1}
                           }, kConductressSilverCost),
            // Portals
            {GatewayType::Portal, MAP_BIRD_ISLAND, {1018, 710}, MAP_TWIN_CITY, {232, 190}},
        }
    },
    {
        MAP_MINE_CAVE, {
            // Portals
            {GatewayType::Portal, MAP_MINE_CAVE, {157, 91}, MAP_TWIN_CITY, {53, 399}},
            {GatewayType::Portal, MAP_MINE_CAVE, {49, 71}, MAP_GROTTO1, {333, 354}},
        }
    },
    {
        MAP_GUILD, {
            // TC
            WithSilverCost({GatewayType::Npc, MAP_GUILD, {354, 345}, MAP_TWIN_CITY, {430, 380}, "GuildKeeper"},
                           kGuildGatewaySilverCost),
            // GC 1
            WithSilverCost({GatewayType::Npc, MAP_GUILD, {331, 338}, MAP_OCEAN, {63, 133}, "GuildConductor1"},
                           kGuildGatewaySilverCost),
        }
    },
    {
        MAP_GROTTO1, {
            {GatewayType::Portal, MAP_GROTTO1, {333, 354}, MAP_MINE_CAVE, {49, 75}},
            {GatewayType::Portal, MAP_GROTTO1, {291, 64}, MAP_GROTTO2, {385, 628}},
            WithSilverCost({GatewayType::Npc, MAP_GROTTO1, {348, 474}, MAP_GUILD, {339, 353}, "GuildTeleporter2"},
                           kGuildGatewaySilverCost)
        }
    },
    {
        MAP_GROTTO2, {
            {GatewayType::Portal, MAP_GROTTO2, {385, 633}, MAP_GROTTO1, {293, 67}},
        }
    },
    // =====================================================================
    // Adventure Zone -- every edge captured by the portal logger 2026-09-02.
    // Tile portals are exact (source = the map file's own portal tile, landing
    // = where the hero appeared). NPC hops carry the NPC's name; dialogue
    // shape (single option vs menu) is NOT yet verified for any of them.
    // =====================================================================
    {
        MAP_NEW_DESERT_CITY, {
            // NPC "Explorer", top-right corner. Logged 2026-09-02:
            //   [portal] 1077 (360,6) -> 1205 (1351,1198)
            {GatewayType::Npc, MAP_NEW_DESERT_CITY, {360, 6}, MAP_ADV_2, {1351, 1198}, "Explorer"},
            // Back to Desert City. Walked three times.
            {GatewayType::Portal, MAP_NEW_DESERT_CITY, {756, 877}, MAP_DESERT_CITY, {159, 467}},
        }
    },
    {
        MAP_ADV_2, {
            {GatewayType::Portal, MAP_ADV_2, {1355, 1198}, MAP_NEW_DESERT_CITY, {362, 12}},
            {GatewayType::Portal, MAP_ADV_2, {3, 160}, MAP_ADV_3, {691, 398}},
        }
    },
    {
        MAP_ADV_3, {
            {GatewayType::Portal, MAP_ADV_3, {3, 301}, MAP_ADV_4, {592, 398}},
            {GatewayType::Portal, MAP_ADV_3, {694, 398}, MAP_ADV_2, {8, 161}},
        }
    },
    {
        MAP_ADV_4, {
            {GatewayType::Portal, MAP_ADV_4, {3, 201}, MAP_BACKUP_CITY, {1072, 798}},
            {GatewayType::Portal, MAP_ADV_4, {594, 397}, MAP_ADV_3, {8, 302}},
        }
    },
    {
        MAP_BACKUP_CITY, {
            {GatewayType::Portal, MAP_BACKUP_CITY, {4, 281}, MAP_ADV_6, {1351, 1199}},
            {GatewayType::Portal, MAP_BACKUP_CITY, {1075, 798}, MAP_ADV_4, {10, 202}},
        }
    },
    {
        MAP_ADV_6, {
            {GatewayType::Portal, MAP_ADV_6, {3, 160}, MAP_ADV_HUB, {950, 478}},
        }
    },
    {
        MAP_ADV_HUB, {
            // four exits: east back to 6, north woods, south canyon, west Meteor Zone
            {GatewayType::Portal, MAP_ADV_HUB, {954, 479}, MAP_ADV_6, {9, 161}},
            {GatewayType::Portal, MAP_ADV_HUB, {480, 3}, MAP_ADV_WOODS, {9, 376}},
            {GatewayType::Portal, MAP_ADV_HUB, {479, 954}, MAP_ADV_7_1, {641, 9}},
            {GatewayType::Portal, MAP_ADV_HUB, {3, 480}, MAP_METEOR_ZONE, {1039, 718}},
            // OldExplorer NPC beside the west portal lands in the same spot.
            {GatewayType::Npc, MAP_ADV_HUB, {12, 476}, MAP_METEOR_ZONE, {1039, 718}, "OldExplorer"},
        }
    },
    {
        MAP_ADV_WOODS, {
            {GatewayType::Portal, MAP_ADV_WOODS, {3, 376}, MAP_ADV_HUB, {481, 10}},
            {GatewayType::Portal, MAP_ADV_WOODS, {938, 567}, MAP_ADV_TASK07, {958, 1211}},
        }
    },
    {
        MAP_ADV_TASK07, {
            {GatewayType::Portal, MAP_ADV_TASK07, {958, 1214}, MAP_ADV_WOODS, {934, 565}},
            {GatewayType::Portal, MAP_ADV_TASK07, {260, 4}, MAP_ADV_TASK08, {9, 502}},
        }
    },
    {
        MAP_ADV_TASK08, {
            {GatewayType::Portal, MAP_ADV_TASK08, {3, 500}, MAP_ADV_TASK07, {261, 10}},
        }
    },
    {
        MAP_ADV_7_1, {
            {GatewayType::Portal, MAP_ADV_7_1, {640, 3}, MAP_ADV_HUB, {479, 950}},
            {GatewayType::Portal, MAP_ADV_7_1, {799, 1434}, MAP_ADV_7_2, {363, 13}},
        }
    },
    {
        MAP_ADV_7_2, {
            // -> back to 7.1 is an NPC at (360,6); name not yet captured -- no entry.
        }
    },
    {
        MAP_METEOR_ZONE, {
            {GatewayType::Portal, MAP_METEOR_ZONE, {4, 329}, MAP_ADV_TASK11, {951, 558}},
            {GatewayType::Npc, MAP_METEOR_ZONE, {719, 1036}, MAP_METEOR_MINE, {29, 70}, "MineSupervisor"},
        }
    },
    {
        MAP_METEOR_MINE, {
            // Exit is by NPC at (28,66); presumed to be MineSupervisor again.
            {GatewayType::Npc, MAP_METEOR_MINE, {28, 66}, MAP_METEOR_ZONE, {715, 1036}, "MineSupervisor"},
        }
    },
    {
        MAP_ADV_TASK11, {
            {GatewayType::Portal, MAP_ADV_TASK11, {3, 400}, MAP_ADV_ISLANDS, {1017, 1294}},
            {GatewayType::Portal, MAP_ADV_TASK11, {955, 558}, MAP_METEOR_ZONE, {8, 329}},
        }
    },
    {
        MAP_ADV_ISLANDS, {
            // Same-map island hops. Every pair walked is two-way and lands you
            // beside the return portal. Reverse halves not yet walked on either
            // All 33 walked on instance 1; instance 2 mirrors it except #32.
            {GatewayType::Portal, MAP_ADV_ISLANDS, {1015, 1295}, MAP_ADV_TASK11, {10, 401}},
            // RANDOM REDIRECT — read before trusting any island edge on this map.
            // Portal use on instance 1 SOMETIMES drops you on instance 2 at a fixed
            // tile, (1003,1278) on its I14, instead of the portal's own destination.
            // Seen from #11 (twice), from #4 (once, right after two normal uses),
            // and once with no portal nearby at (583,825). The normal destination is
            // the one wired below; a routing step that lands on map 1219 instead is
            // this mechanic, not a wrong table entry — re-plan from there (its #32
            // returns to this map's I0 at (378,37), beside Grandpa).
            {GatewayType::Portal, MAP_ADV_ISLANDS, {375, 153}, MAP_ADV_ISLANDS, {310, 174}},   // #11  I0 -> I1
            // NPC "Grandpa" on I0 at ~(378,32) — same spot the Boatman occupies on
            // instance 2 — is the other door to Adventure Islands 2. Landed at
            // (1016,1292) once and (1003,1278) once; both on AZ2's I14.
            {GatewayType::Npc, MAP_ADV_ISLANDS, {378, 32}, MAP_ADV_ISLANDS2, {1016, 1292}, "Grandpa"},
            {GatewayType::Portal, MAP_ADV_ISLANDS, {906, 1041}, MAP_ADV_ISLANDS, {942, 1012}},   // #27
            {GatewayType::Portal, MAP_ADV_ISLANDS, {942, 1015}, MAP_ADV_ISLANDS, {903, 1041}},   // #26
            {GatewayType::Portal, MAP_ADV_ISLANDS, {1025, 1038}, MAP_ADV_ISLANDS, {1087, 1124}},   // #28
            {GatewayType::Portal, MAP_ADV_ISLANDS, {1083, 1120}, MAP_ADV_ISLANDS, {1022, 1035}},   // #29
            {GatewayType::Portal, MAP_ADV_ISLANDS, {896, 891}, MAP_ADV_ISLANDS, {871, 853}},   // #23
            {GatewayType::Portal, MAP_ADV_ISLANDS, {871, 856}, MAP_ADV_ISLANDS, {899, 894}},   // #22
            {GatewayType::Portal, MAP_ADV_ISLANDS, {872, 847}, MAP_ADV_ISLANDS, {900, 826}},   // #19
            {GatewayType::Portal, MAP_ADV_ISLANDS, {898, 828}, MAP_ADV_ISLANDS, {872, 850}},   // #18
            {GatewayType::Portal, MAP_ADV_ISLANDS, {861, 846}, MAP_ADV_ISLANDS, {808, 823}},   // #20
            {GatewayType::Portal, MAP_ADV_ISLANDS, {811, 826}, MAP_ADV_ISLANDS, {864, 846}},   // #21
            {GatewayType::Portal, MAP_ADV_ISLANDS, {781, 841}, MAP_ADV_ISLANDS, {705, 921}},   // #0
            {GatewayType::Portal, MAP_ADV_ISLANDS, {579, 824}, MAP_ADV_ISLANDS, {539, 752}},   // #2
            {GatewayType::Portal, MAP_ADV_ISLANDS, {476, 688}, MAP_ADV_ISLANDS, {369, 651}},   // #4
            {GatewayType::Portal, MAP_ADV_ISLANDS, {352, 512}, MAP_ADV_ISLANDS, {404, 451}},   // #6
            {GatewayType::Portal, MAP_ADV_ISLANDS, {404, 454}, MAP_ADV_ISLANDS, {349, 512}},   // #7
            {GatewayType::Portal, MAP_ADV_ISLANDS, {392, 387}, MAP_ADV_ISLANDS, {335, 282}},   // #8
            {GatewayType::Portal, MAP_ADV_ISLANDS, {310, 171}, MAP_ADV_ISLANDS, {378, 153}},   // #10
            {GatewayType::Portal, MAP_ADV_ISLANDS, {614, 314}, MAP_ADV_ISLANDS, {623, 402}},   // #12
            {GatewayType::Portal, MAP_ADV_ISLANDS, {620, 399}, MAP_ADV_ISLANDS, {611, 311}},   // #13
            {GatewayType::Portal, MAP_ADV_ISLANDS, {674, 415}, MAP_ADV_ISLANDS, {713, 377}},   // #14
            {GatewayType::Portal, MAP_ADV_ISLANDS, {710, 377}, MAP_ADV_ISLANDS, {674, 418}},   // #15
            {GatewayType::Portal, MAP_ADV_ISLANDS, {837, 458}, MAP_ADV_ISLANDS, {846, 558}},   // #16
            {GatewayType::Portal, MAP_ADV_ISLANDS, {846, 555}, MAP_ADV_ISLANDS, {834, 455}},   // #17
            {GatewayType::Portal, MAP_ADV_ISLANDS, {810, 677}, MAP_ADV_ISLANDS, {690, 667}},   // #24
            {GatewayType::Portal, MAP_ADV_ISLANDS, {693, 667}, MAP_ADV_ISLANDS, {813, 677}},   // #25
            {GatewayType::Portal, MAP_ADV_ISLANDS, {1049, 838}, MAP_ADV_ISLANDS, {1138, 895}},   // #30  I7 -> I13
            {GatewayType::Portal, MAP_ADV_ISLANDS, {1135, 892}, MAP_ADV_ISLANDS, {1046, 835}},   // #31  I13 -> I7
            {GatewayType::Portal, MAP_ADV_ISLANDS, {708, 921}, MAP_ADV_ISLANDS, {781, 838}},   // #1   I10 -> I9
            {GatewayType::Portal, MAP_ADV_ISLANDS, {542, 755}, MAP_ADV_ISLANDS, {582, 827}},   // #3   I8 -> I10
            {GatewayType::Portal, MAP_ADV_ISLANDS, {372, 651}, MAP_ADV_ISLANDS, {479, 691}},   // #5   I3 -> I8
            {GatewayType::Portal, MAP_ADV_ISLANDS, {338, 285}, MAP_ADV_ISLANDS, {395, 390}},   // #9   I1 -> I4
        }
    },
    {
        MAP_ADV_ISLANDS2, {
            // Same file, same portal numbers as Adventure Islands 1; wired the
            // same except #11 (I0<->I1 here) and the exit, which is the Boatman.
            // #32 here does NOT go to task11 like instance 1's — it drops you on
            // instance 1's I0 at (378,37), right beside Grandpa. Walked twice.
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {1015, 1295}, MAP_ADV_ISLANDS, {378, 37}},
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {375, 153}, MAP_ADV_ISLANDS2, {310, 174}},
            {GatewayType::Npc, MAP_ADV_ISLANDS2, {386, 40}, MAP_BACKUP_CITY, {448, 272}, "Boatman"},
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {906, 1041}, MAP_ADV_ISLANDS2, {942, 1012}},   // #27
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {942, 1015}, MAP_ADV_ISLANDS2, {903, 1041}},   // #26
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {1025, 1038}, MAP_ADV_ISLANDS2, {1087, 1124}},   // #28
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {1083, 1120}, MAP_ADV_ISLANDS2, {1022, 1035}},   // #29
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {896, 891}, MAP_ADV_ISLANDS2, {871, 853}},   // #23
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {871, 856}, MAP_ADV_ISLANDS2, {899, 894}},   // #22
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {872, 847}, MAP_ADV_ISLANDS2, {900, 826}},   // #19
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {898, 828}, MAP_ADV_ISLANDS2, {872, 850}},   // #18
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {861, 846}, MAP_ADV_ISLANDS2, {808, 823}},   // #20
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {811, 826}, MAP_ADV_ISLANDS2, {864, 846}},   // #21
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {781, 841}, MAP_ADV_ISLANDS2, {705, 921}},   // #0
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {579, 824}, MAP_ADV_ISLANDS2, {539, 752}},   // #2
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {476, 688}, MAP_ADV_ISLANDS2, {369, 651}},   // #4
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {352, 512}, MAP_ADV_ISLANDS2, {404, 451}},   // #6
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {404, 454}, MAP_ADV_ISLANDS2, {349, 512}},   // #7
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {392, 387}, MAP_ADV_ISLANDS2, {335, 282}},   // #8
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {310, 171}, MAP_ADV_ISLANDS2, {378, 153}},   // #10
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {614, 314}, MAP_ADV_ISLANDS2, {623, 402}},   // #12
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {620, 399}, MAP_ADV_ISLANDS2, {611, 311}},   // #13
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {674, 415}, MAP_ADV_ISLANDS2, {713, 377}},   // #14
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {710, 377}, MAP_ADV_ISLANDS2, {674, 418}},   // #15
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {837, 458}, MAP_ADV_ISLANDS2, {846, 558}},   // #16
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {846, 555}, MAP_ADV_ISLANDS2, {834, 455}},   // #17
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {810, 677}, MAP_ADV_ISLANDS2, {690, 667}},   // #24
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {693, 667}, MAP_ADV_ISLANDS2, {813, 677}},   // #25
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {1049, 838}, MAP_ADV_ISLANDS2, {1138, 895}},   // #30  I7 -> I13
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {1135, 892}, MAP_ADV_ISLANDS2, {1046, 835}},   // #31  I13 -> I7
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {708, 921}, MAP_ADV_ISLANDS2, {781, 838}},   // #1   I10 -> I9
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {542, 755}, MAP_ADV_ISLANDS2, {582, 827}},   // #3   I8 -> I10
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {372, 651}, MAP_ADV_ISLANDS2, {479, 691}},   // #5   I3 -> I8
            {GatewayType::Portal, MAP_ADV_ISLANDS2, {338, 285}, MAP_ADV_ISLANDS2, {395, 390}},   // #9   I1 -> I4
        }
    },
    {
        MAP_OCEAN, {
            WithSilverCost({GatewayType::Npc, MAP_OCEAN, {67, 119}, MAP_GUILD, {351, 341}, "GuildTeleporter1"},
                           kGuildGatewaySilverCost)
        }
    }
};

// =====================================================================
// Named destinations (map ID + target position)
// =====================================================================
static const Destination s_destinations[] = {
    {"Twin City", MAP_TWIN_CITY, {430, 380}},
    {"Desert City", MAP_DESERT_CITY, {493, 650}},
    {"Mystic Castle", MAP_MYSTIC_CASTLE, {312, 646}},
    {"Phoenix Castle", MAP_PHOENIX_CASTLE, {193, 266}},
    {"Ape Mountain", MAP_APE_MOUNTAIN, {566, 565}},
    {"Ape City 2", MAP_APE_CITY2, {361, 6}},
    {"Phoenix Castle 2", MAP_PHOENIX_CASTLE2, {11, 361}},
    {"Dreamland", MAP_DREAMLAND, {532, 529}},
    {"New Desert City", MAP_NEW_DESERT_CITY, {362, 12}},
    {"Backup City", MAP_BACKUP_CITY, {448, 272}},
    {"Adventure Hub", MAP_ADV_HUB, {950, 478}},
    {"Meteor Zone", MAP_METEOR_ZONE, {1039, 718}},
    {"Adventure Islands", MAP_ADV_ISLANDS, {1017, 1294}},
    {"Adventure Islands 2", MAP_ADV_ISLANDS2, {1003, 1278}},
    {"Bird Island", MAP_BIRD_ISLAND, {717, 577}},
    {"Mine Cave", MAP_MINE_CAVE, {157, 91}},
    {"Market", MAP_MARKET, {211, 196}},
    {"Guild Area", MAP_GUILD, {351, 341}},
    {"Frozen Grotto 1", MAP_GROTTO1, {356, 482}},
    {"Frozen Grotto 2", MAP_GROTTO2, {385, 628}}
};

const Destination* GetDestinations(size_t& count)
{
    count = sizeof(s_destinations) / sizeof(s_destinations[0]);
    return s_destinations;
}

const char* GetMapName(OBJID mapId)
{
    for (const auto& d : s_destinations)
        if (d.mapId == mapId) return d.name;
    return "Unknown";
}

static const std::vector<Gateway> s_empty;

const std::vector<Gateway>& GetGateways(OBJID mapId)
{
    auto it = s_gateways.find(mapId);
    return it != s_gateways.end() ? it->second : s_empty;
}

// =====================================================================
// Dijkstra pathfinding through gateway graph (minimizes walking distance)
// =====================================================================

struct GwNode
{
    OBJID mapId;
    Position pos; // current position on this map
    int walkCost; // cumulative walking distance (Manhattan)
    uint32_t silverLeft; // remaining silver after paid gateways
    std::vector<Gateway> path;
    bool atGoal = false;

    bool operator>(const GwNode& o) const { return walkCost > o.walkCost; }
};

// Key: (mapId, quantized position) — distinguishes different regions on the same map
struct GwNodeKey
{
    OBJID mapId;
    uint32_t qx;
    uint32_t qy;
    uint32_t silverLeft;

    bool operator==(const GwNodeKey& other) const
    {
        return mapId == other.mapId
            && qx == other.qx
            && qy == other.qy
            && silverLeft == other.silverLeft;
    }
};

struct GwNodeKeyHash
{
    size_t operator()(const GwNodeKey& key) const
    {
        size_t h = static_cast<size_t>(key.mapId);
        h ^= static_cast<size_t>(key.qx) << 8;
        h ^= static_cast<size_t>(key.qy) << 24;
        h ^= static_cast<size_t>(key.silverLeft) << 1;
        return h;
    }
};

static inline GwNodeKey GwNodeStateKey(OBJID mapId, const Position& pos, uint32_t silverLeft)
{
    uint32_t qx = (uint32_t)(pos.x / 32);
    uint32_t qy = (uint32_t)(pos.y / 32);
    return {mapId, qx, qy, silverLeft};
}

static inline int ManhattanDist(const Position& a, const Position& b)
{
    return abs(a.x - b.x) + abs(a.y - b.y);
}

static void AppendVipTeleportGateways(std::vector<Gateway>& gateways, OBJID mapId)
{
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_TWIN_CITY, {430, 380}});
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_PHOENIX_CASTLE, {193, 266}});
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_APE_MOUNTAIN, {566, 565}});
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_DESERT_CITY, {493, 650}});
    gateways.push_back({GatewayType::VipTeleport, mapId, {0, 0}, MAP_BIRD_ISLAND, {717, 577}});
}

static bool HasItemGatewayType(const std::vector<uint32_t>& availableItemTypes, uint32_t itemTypeId)
{
    return std::find(availableItemTypes.begin(), availableItemTypes.end(), itemTypeId) != availableItemTypes.end();
}

static void AppendItemGateways(std::vector<Gateway>& gateways, OBJID mapId,
                               const std::vector<uint32_t>& availableItemTypes)
{
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::TWIN_CITY_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_TWIN_CITY, {430, 380}, nullptr, 0, {}, 101, 0,
            ItemTypeId::TWIN_CITY_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::DESERT_CITY_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_DESERT_CITY, {493, 650}, nullptr, 0, {}, 101, 0,
            ItemTypeId::DESERT_CITY_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::APE_CITY_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_APE_MOUNTAIN, {566, 565}, nullptr, 0, {}, 101, 0,
            ItemTypeId::APE_CITY_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::CASTLE_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_PHOENIX_CASTLE, {193, 266}, nullptr, 0, {}, 101, 0,
            ItemTypeId::CASTLE_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::BIRD_ISLAND_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_BIRD_ISLAND, {717, 577}, nullptr, 0, {}, 101, 0,
            ItemTypeId::BIRD_ISLAND_GATE
        });
    if (HasItemGatewayType(availableItemTypes, ItemTypeId::STONE_CITY_GATE))
        gateways.push_back({
            GatewayType::Item, mapId, {0, 0}, MAP_MYSTIC_CASTLE, {312, 646}, nullptr, 0, {}, 101, 0,
            ItemTypeId::STONE_CITY_GATE
        });
}

std::vector<Gateway> FindGatewayPath(
    OBJID from,
    OBJID to,
    Position heroPos,
    uint32_t availableSilver,
    bool allowVipTeleport,
    Position finalPos,
    std::vector<uint32_t> availableItemTypes)
{
    const bool hasFinalPos = finalPos.x != 0 || finalPos.y != 0;

    std::priority_queue<GwNode, std::vector<GwNode>, std::greater<GwNode>> open;
    std::unordered_map<GwNodeKey, int, GwNodeKeyHash> bestCost;
    int bestGoalCost = INT_MAX;

    open.push({from, heroPos, 0, availableSilver, {}});

    while (!open.empty())
    {
        GwNode current = std::move(const_cast<GwNode&>(open.top()));
        open.pop();

        if (current.atGoal)
            return current.path;

        const GwNodeKey curKey = GwNodeStateKey(current.mapId, current.pos, current.silverLeft);
        auto it = bestCost.find(curKey);
        if (it != bestCost.end() && current.walkCost > it->second)
            continue;

        if (current.mapId == to)
        {
            const int goalCost = current.walkCost + (hasFinalPos ? ManhattanDist(current.pos, finalPos) : 0);
            if (goalCost < bestGoalCost)
            {
                bestGoalCost = goalCost;
                open.push({current.mapId, current.pos, goalCost, current.silverLeft, current.path, true});
            }
        }

        std::vector<Gateway> gateways = GetGateways(current.mapId);
        if (allowVipTeleport)
            AppendVipTeleportGateways(gateways, current.mapId);
        if (!availableItemTypes.empty())
            AppendItemGateways(gateways, current.mapId, availableItemTypes);

        for (auto& gw : gateways)
        {
            // Skip Market outbound Mark.Controller routes when Market is not the
            // origin. Instant gateways remain valid from Market.
            if (current.mapId == MAP_MARKET && from != MAP_MARKET && !gw.IsInstant())
                continue;
            if (gw.silverCost > current.silverLeft)
                continue;
            if (gw.type == GatewayType::Item
                && gw.destMapId == current.mapId
                && gw.HasDestPos()
                && ManhattanDist(current.pos, gw.destPos) < kMinSameMapItemGateDistance)
            {
                continue;
            }

            // Cost = walking distance from current position to this gateway.
            // VIP teleports leaving the origin map get a penalty so the pathfinder
            // prefers staying on the hero's current map when multiple routes exist.
            int stepCost = gw.IsInstant() ? 0 : ManhattanDist(current.pos, gw.pos);
            if (gw.type == GatewayType::VipTeleport && gw.destMapId != from)
                stepCost += 500;
            int newCost = current.walkCost + stepCost;
            uint32_t remainingSilver = current.silverLeft - gw.silverCost;

            OBJID dest = gw.destMapId;
            Position landingPos = gw.HasDestPos() ? gw.destPos : Position{0, 0};

            GwNodeKey key = GwNodeStateKey(dest, landingPos, remainingSilver);
            auto dit = bestCost.find(key);
            if (dit != bestCost.end() && newCost >= dit->second)
                continue;
            bestCost[key] = newCost;

            std::vector<Gateway> newPath = current.path;
            newPath.push_back(gw);

            open.push({dest, landingPos, newCost, remainingSilver, std::move(newPath)});
        }
    }

    return {}; // no path found
}
