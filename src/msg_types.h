#pragma once
#include <cstdint>

// =====================================================================
// msg_types.h — Conquer Online message-type ID -> name lookup, for
// labeling raw packet captures (overlay Packets tab, RelayLogger hex
// dumps). Display-only: nothing here is used for any protocol decision.
//
// Source: the community-maintained CO Development Wiki
// (https://conquer-online.github.io/wiki/network/messages/index.html),
// covering patches 4267-5187+. Trusting these IDs for THIS client rests
// on one confirmed data point: this project's own kMsgActionPacketType
// (packets.cpp, 0x03F2 = 1010) matches the wiki's documented "MsgAction"
// exactly. That's evidence the message-type ID space and the 4-byte
// [u16 size][u16 type] header are unchanged from stock TQ protocol --
// it is NOT evidence the crypto layer matches (see
// docs/investigation/CONNECTION_DECIPHER_PREP.md, which is still open).
//
// A handful of IDs are legitimately reused across different TQ patches
// for unrelated messages (the wiki documents several of these, e.g. 1102
// = MsgAccountSoftKb in one patch, MsgPackage in another); only one name
// is kept per ID here, so a match is a best-effort hint for a human
// reading a capture, not a guarantee of which patch-specific message it
// actually is.
// =====================================================================

inline const char* MsgTypeName(uint16_t type)
{
    switch (type) {
        case 1001: return "MsgRegister";
        case 1004: return "MsgTalk";
        case 1005: return "MsgWalk";
        case 1006: return "MsgUserInfo";
        case 1008: return "MsgItemInfo";
        case 1009: return "MsgItem";
        case 1010: return "MsgAction";
        case 1012: return "MsgTick";
        case 1014: return "MsgPlayer";
        case 1015: return "MsgName";
        case 1016: return "MsgWeather";
        case 1017: return "MsgUserAttrib";
        case 1019: return "MsgFriend";
        case 1022: return "MsgInteract";
        case 1023: return "MsgTeam";
        case 1024: return "MsgAllot";
        case 1025: return "MsgWeaponSkill";
        case 1026: return "MsgTeamMember";
        case 1027: return "MsgGemEmbed";
        case 1028: return "MsgFuse";
        case 1029: return "MsgTeamAward";
        case 1032: return "MsgBattleEffectiveness";
        case 1033: return "MsgData";
        case 1034: return "MsgDetainItemInfo";
        case 1036: return "MsgGodExp";
        case 1037: return "MsgPing";
        case 1038: return "MsgSolidify";
        case 1039: return "MsgNpcPath";
        case 1040: return "MsgPlayerAttribInfo";
        case 1041: return "MsgEnemyList";
        case 1042: return "MsgMonsterTransform";
        case 1043: return "MsgTeamRoll";
        case 1044: return "MsgLoadMap";
        case 1045: return "MsgMailOperation";
        case 1046: return "MsgMailList";
        case 1047: return "MsgMailNotify";
        case 1048: return "MsgMailContent";
        case 1049: return "MsgPCServerConfig";
        case 1051: return "MsgAccount";
        case 1052: return "MsgConnect";
        case 1055: return "MsgConnectEx";
        case 1056: return "MsgTrade";
        case 1057: return "MsgConnectWithBgp";
        case 1058: return "MsgSynpOffer";
        case 1059: return "MsgEncryptCode";
        case 1061: return "MsgDutyMinContri";
        case 1062: return "MsgSynCompete";
        case 1063: return "MsgSelfSynMemAwardRank";
        case 1064: return "MsgSponsor";
        case 1065: return "MsgSponsorInfo";
        case 1066: return "MsgMeteSpecial";
        case 1067: return "MsgLoginNotice";
        case 1070: return "MsgHangUp";
        case 1071: return "MsgCompleteRank";
        case 1072: return "MsgCampFight";
        case 1073: return "MsgCampFightInfo";
        case 1074: return "MsgPicKeyError";
        case 1075: return "MsgPicKey";
        case 1081: return "MsgCheatingProgram";
        case 1083: return "MsgRequestKeyLogin";
        case 1084: return "MsgConfirmKeyLogin";
        case 1086: return "MsgAccount";
        case 1090: return "MsgLoginAccountEx";
        case 1098: return "MsgConfirmKeyLoginMobile";
        case 1100: return "MsgPCNum";
        case 1101: return "MsgMapItem";
        case 1102: return "MsgAccountSoftKb"; // aka MsgPackage in some patches
        case 1103: return "MsgMagicInfo";
        case 1104: return "MsgFlushExp";
        case 1105: return "MsgMagicEffect";
        case 1106: return "MsgSyndicateAttributeInfo";
        case 1107: return "MsgSyndicate";
        case 1108: return "MsgItemInfoEx";
        case 1109: return "MsgNpcInfoEx";
        case 1110: return "MsgMapInfo";
        case 1111: return "MsgMessageBoard";
        case 1112: return "MsgSynMemberInfo";
        case 1113: return "MsgDice";
        case 1114: return "MsgSyncAction";
        case 1115: return "MsgDisconnect";
        case 1121: return "MsgFacebookAccount";
        case 1124: return "MsgAccountSRP6";
        case 1125: return "MsgAccountKalydo";
        case 1126: return "MsgInviteTrans";
        case 1127: return "MsgMentorPlayer";
        case 1128: return "MsgVipUserHandle";
        case 1129: return "MsgVipFunctionValidNotify";
        case 1130: return "MsgTitle";
        case 1134: return "MsgTaskStatus";
        case 1135: return "MsgTaskDetailInfo";
        case 1136: return "MsgAchievement";
        case 1150: return "MsgFlower";
        case 1151: return "MsgRank";
        case 1202: return "MsgRegisterFaceBook";
        case 1203: return "MsgConnectFaceBook";
        case 1213: return "MsgLoginChallengeS";
        case 1214: return "MsgLoginProofC";
        case 1312: return "MsgFamily";
        case 1313: return "MsgFamilyOccupy";
        case 1314: return "MsgLottery";
        case 1315: return "MsgOperatingAct";
        case 1316: return "MsgOperatingActInfo";
        case 1320: return "MsgAuction";
        case 1321: return "MsgAuctionItem";
        case 1322: return "MsgAuctionQuery";
        case 1323: return "MsgPromotionAct";
        case 1324: return "MsgPromotionInfo";
        case 1350: return "MsgGameServerShutDown";
        case 1351: return "MsgSlotAction";
        case 1352: return "MsgSlotResult";
        case 1518: return "MsgConnectLegalitySpec";
        case 1542: return "MsgAccountSRP6Ex";
        case 1636: return "MsgAccountSRP6Ex";
        case 2030: return "MsgNpcInfo";
        case 2031: return "MsgNpc";
        case 2032: return "MsgTaskDialog";
        case 2033: return "MsgFriendInfo";
        case 2035: return "MsgPetInfo";
        case 2036: return "MsgDataArray";
        case 2041: return "MsgAnnounceList";
        case 2042: return "MsgAnnounceInfo";
        case 2043: return "MsgTrainingInfo";
        case 2044: return "MsgTraining";
        case 2045: return "MsgAuraGroup";
        case 2046: return "MsgTradeBuddy";
        case 2047: return "MsgTradeBuddyInfo";
        case 2048: return "MsgEquipLock";
        case 2050: return "MsgPigeon";
        case 2051: return "MsgPigeonQuery";
        case 2064: return "MsgPeerage";
        case 2065: return "MsgGuide";
        case 2066: return "MsgGuideInfo";
        case 2067: return "MsgContribute";
        case 2068: return "MsgQuiz";
        case 2069: return "MsgQuizSponsor";
        case 2070: return "MsgSuitStatus";
        case 2071: return "MsgRelation";
        case 2072: return "MsgRaceTrackProp";
        case 2073: return "MsgRaceTrackPropEffect";
        case 2075: return "MsgRaceTrackStatus";
        case 2076: return "MsgQuench";
        case 2077: return "MsgItemStatus";
        case 2078: return "MsgUserIPInfo";
        case 2079: return "MsgServerInfo";
        case 2080: return "MsgChangeName";
        case 2081: return "MsgDeadMark";
        case 2082: return "MsgUserCityInfo";
        case 2090: return "MsgShowHandEnter";
        case 2091: return "MsgShowHandDealtCard";
        case 2092: return "MsgShowHandActivePlayer";
        case 2093: return "MsgShowHandCallAction";
        case 2094: return "MsgShowHandLayCard";
        case 2095: return "MsgShowHandGameResult";
        case 2096: return "MsgShowHandExit";
        case 2097: return "MsgShowHandOnlineStatus";
        case 2098: return "MsgShowHandLostInfo";
        case 2099: return "MsgShowHandTrusteeship";
        case 2101: return "MsgFactionRankInfo";
        case 2102: return "MsgSynMemberList";
        case 2103: return "MsgSynChgDomName";
        case 2110: return "MsgSuperFlag";
        case 2170: return "MsgLeaveWord";
        case 2171: return "MsgTexasInteractive";
        case 2172: return "MsgTexasNpcInfo";
        case 2201: return "MsgTotemPoleInfo";
        case 2202: return "MsgWeaponsInfo";
        case 2203: return "MsgTotemPole";
        case 2205: return "MsgQualifyingInteractive";
        case 2206: return "MsgQualifyingFightersList";
        case 2207: return "MsgQualifyingRank";
        case 2208: return "MsgQualifyingSeasonRankList";
        case 2209: return "MsgQualifyingDetailInfo";
        case 2210: return "MsgArenicScore";
        case 2211: return "MsgArenicWitness";
        case 2218: return "MsgElitePKArenic";
        case 2219: return "MsgPKEliteMatchInfo";
        case 2220: return "MsgPKStatistic";
        case 2221: return "MsgPKEnable";
        case 2222: return "MsgElitePKScore";
        case 2223: return "MsgElitePKGameRankInfo";
        case 2224: return "MsgWarFlag";
        case 2225: return "MsgSynRecruitAdvertising";
        case 2226: return "MsgSynRecruitAdvertisingList";
        case 2227: return "MsgSynRecruitAdvertisingOpt";
        case 2230: return "MsgTeamPKArenic";
        case 2231: return "MsgTeamPKArenicScore";
        case 2232: return "MsgTeamPKMatchInfo";
        case 2233: return "MsgTeamPKRankInfo";
        case 2240: return "MsgDominateTeamName";
        case 2241: return "MsgTeamArenaInteractive";
        case 2242: return "MsgTeamArenaFightingTeamList";
        case 2243: return "MsgTeamArenaRank";
        case 2244: return "MsgTeamArenaYTop10List";
        case 2245: return "MsgTeamArenaHeroData";
        case 2246: return "MsgTeamArenaScore";
        case 2247: return "MsgTeamArenaFightingMemberInfo";
        case 2250: return "MsgTeamPopPKArenic";
        case 2251: return "MsgTeamPopPKArenicScore";
        case 2252: return "MsgTeamPopPKMatchInfo";
        case 2253: return "MsgTeamPopPKRankInfo";
        case 2260: return "MsgDominateTeamPopPkName";
        case 2261: return "Msg2ndPsw";
        case 2262: return "MsgPaint";
        case 2286: return "MsgMapItem";
        case 2320: return "MsgSubPro";
        case 2330: return "MsgFactionMatch";
        case 2331: return "MsgFMRoundRobin";
        case 2332: return "MsgFMMatch";
        case 2333: return "MsgFactionMatchWitness";
        case 2400: return "MsgTransportor";
        case 2401: return "MsgInstance";
        case 2410: return "MsgAura";
        case 2420: return "MsgVerifyCheck";
        case 2430: return "MsgNationality";
        case 2501: return "MsgCrossSwitch";
        case 2502: return "MsgCrossFlagWar";
        case 2504: return "MsgCrossFlagWarMerit";
        case 2505: return "MsgCrossFlagWarAltar";
        case 2506: return "MsgCrossFlagWarFlag";
        case 2507: return "MsgCrossFlagWarRank";
        case 2510: return "MsgFactionChiefBase";
        case 2521: return "MsgKickOut";
        case 2533: return "MsgTrainingVitality";
        case 2534: return "MsgTrainingVitalityInfo";
        case 2535: return "MsgTrainingVitalityScore";
        case 2600: return "MsgGLRankingList";
        case 2601: return "MsgGLHeroDetail";
        case 2602: return "MsgGLLastSeasonTopScore";
        case 2603: return "MsgGLChampionList";
        case 2604: return "MsgGLInteractive";
        case 2700: return "MsgOwnKongfuBase";
        case 2701: return "MsgOwnKongfuImproveSummaryInfo";
        case 2702: return "MsgOwnKongfuImproveFeedback";
        case 2703: return "MsgOwnKongRank";
        case 2704: return "MsgOwnKongfuPKSetting";
        case 2710: return "MsgMagicCoat";
        case 3200: return "MsgSignIn";
        // 5-digit alternate IDs used for the same message in later client
        // versions (wiki notes these in brackets next to the 4-digit ID).
        case 10005: return "MsgWalk";
        case 10010: return "MsgAction";
        case 10014: return "MsgPlayer";
        case 10017: return "MsgUserAttrib";
        default: return nullptr;
    }
}
