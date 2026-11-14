/* Star table, star patterns and the equatorial to world rotation for the sky scene layouts.
 *
 * The table and the figures are ported by script from the approved mockup
 * (dir_pick.js, STARS and FIGURES), J2000. ra_h in hours, dec_deg in degrees,
 * mag visual magnitude, bv the B-V colour index. Pure, float only, header only.
 */
#pragma once

#include <stdint.h>
#include "sky_scene_math.h"

#define SKY_STAR_N     237
#define SKY_FIG_N      16
#define SKY_FIG_RUN_N  31

typedef struct { float ra_h, dec_deg, mag, bv; } sky_star_t;

static const sky_star_t SKY_STARS[SKY_STAR_N] = {
    { 6.7525f, -16.716f, -1.46f, 0.00f }, /* 0 Sirius */
    { 6.3992f, -52.696f, -0.74f, 0.15f }, /* 1 Canopus */
    { 14.2610f, 19.182f, -0.05f, 1.23f }, /* 2 Arcturus */
    { 14.6600f, -60.835f, -0.27f, 0.71f }, /* 3 RigilKent */
    { 18.6156f, 38.784f, 0.03f, 0.00f }, /* 4 Vega */
    { 5.2782f, 45.998f, 0.08f, 0.80f }, /* 5 Capella */
    { 5.2423f, -8.202f, 0.13f, -0.03f }, /* 6 Rigel */
    { 7.6550f, 5.225f, 0.34f, 0.42f }, /* 7 Procyon */
    { 1.6286f, -57.237f, 0.46f, -0.16f }, /* 8 Achernar */
    { 5.9195f, 7.407f, 0.50f, 1.85f }, /* 9 Betelgeuse */
    { 14.0637f, -60.373f, 0.61f, -0.23f }, /* 10 Hadar */
    { 19.8464f, 8.868f, 0.76f, 0.22f }, /* 11 Altair */
    { 12.4433f, -63.099f, 0.77f, -0.24f }, /* 12 Acrux */
    { 4.5987f, 16.509f, 0.86f, 1.54f }, /* 13 Aldebaran */
    { 16.4901f, -26.432f, 0.96f, 1.83f }, /* 14 Antares */
    { 13.4199f, -11.161f, 0.97f, -0.23f }, /* 15 Spica */
    { 7.7553f, 28.026f, 1.14f, 1.00f }, /* 16 Pollux */
    { 22.9608f, -29.622f, 1.16f, 0.09f }, /* 17 Fomalhaut */
    { 20.6905f, 45.280f, 1.25f, 0.09f }, /* 18 Deneb */
    { 12.7954f, -59.689f, 1.25f, -0.24f }, /* 19 Mimosa */
    { 10.1395f, 11.967f, 1.40f, -0.11f }, /* 20 Regulus */
    { 6.9771f, -28.972f, 1.50f, -0.21f }, /* 21 Adhara */
    { 7.5767f, 31.888f, 1.58f, 0.03f }, /* 22 Castor */
    { 12.5194f, -57.113f, 1.63f, 1.60f }, /* 23 Gacrux */
    { 17.5601f, -37.104f, 1.63f, -0.22f }, /* 24 Shaula */
    { 5.4189f, 6.350f, 1.64f, -0.22f }, /* 25 Bellatrix */
    { 5.4382f, 28.608f, 1.65f, -0.13f }, /* 26 Elnath */
    { 5.6036f, -1.202f, 1.69f, -0.18f }, /* 27 Alnilam */
    { 22.1372f, -46.961f, 1.74f, -0.07f }, /* 28 Alnair */
    { 5.6793f, -1.943f, 1.77f, -0.20f }, /* 29 Alnitak */
    { 12.9005f, 55.960f, 1.77f, -0.02f }, /* 30 Alioth */
    { 11.0621f, 61.751f, 1.79f, 1.07f }, /* 31 Dubhe */
    { 3.4054f, 49.861f, 1.79f, 0.48f }, /* 32 Mirfak */
    { 7.1399f, -26.393f, 1.83f, 0.68f }, /* 33 Wezen */
    { 18.4029f, -34.385f, 1.85f, -0.03f }, /* 34 KausAustralis */
    { 13.7923f, 49.313f, 1.86f, -0.19f }, /* 35 Alkaid */
    { 17.6219f, -42.998f, 1.87f, 0.40f }, /* 36 Sargas */
    { 5.9921f, 44.948f, 1.90f, 0.08f }, /* 37 Menkalinan */
    { 6.6285f, 16.399f, 1.93f, 0.00f }, /* 38 Alhena */
    { 2.5303f, 89.264f, 1.98f, 0.60f }, /* 39 Polaris */
    { 6.3783f, -17.956f, 1.98f, -0.23f }, /* 40 Mirzam */
    { 9.4598f, -8.659f, 1.98f, 1.44f }, /* 41 Alphard */
    { 2.1196f, 23.462f, 2.00f, 1.15f }, /* 42 Hamal */
    { 10.3329f, 19.842f, 2.01f, 1.13f }, /* 43 Algieba */
    { 0.7265f, -17.987f, 2.04f, 1.02f }, /* 44 Diphda */
    { 18.9211f, -26.297f, 2.05f, -0.22f }, /* 45 Nunki */
    { 14.1114f, -36.370f, 2.06f, 1.01f }, /* 46 Menkent */
    { 1.1622f, 35.621f, 2.06f, 1.58f }, /* 47 Mirach */
    { 0.1398f, 29.091f, 2.06f, -0.11f }, /* 48 Alpheratz */
    { 17.5822f, 12.560f, 2.08f, 0.15f }, /* 49 Rasalhague */
    { 14.8451f, 74.156f, 2.08f, 1.47f }, /* 50 Kochab */
    { 5.7959f, -9.670f, 2.09f, -0.17f }, /* 51 Saiph */
    { 2.0650f, 42.330f, 2.10f, 1.37f }, /* 52 Almach */
    { 3.1361f, 40.956f, 2.12f, -0.05f }, /* 53 Algol */
    { 11.8177f, 14.572f, 2.14f, 0.09f }, /* 54 Denebola */
    { 22.7111f, -46.885f, 2.10f, 1.60f }, /* 55 BetaGru */
    { 5.5334f, -0.299f, 2.23f, -0.22f }, /* 56 Mintaka */
    { 15.5781f, 26.715f, 2.23f, -0.02f }, /* 57 Alphecca */
    { 20.3705f, 40.257f, 2.23f, 0.67f }, /* 58 Sadr */
    { 13.3988f, 54.925f, 2.23f, 0.02f }, /* 59 Mizar */
    { 0.6751f, 56.537f, 2.24f, 1.17f }, /* 60 Schedar */
    { 17.9434f, 51.489f, 2.24f, 1.52f }, /* 61 Eltanin */
    { 8.0597f, -40.003f, 2.25f, -0.27f }, /* 62 Naos */
    { 0.1529f, 59.150f, 2.28f, 0.34f }, /* 63 Caph */
    { 16.0056f, -22.622f, 2.29f, -0.12f }, /* 64 Dschubba */
    { 16.8361f, -34.293f, 2.29f, 1.15f }, /* 65 EpsSco */
    { 14.7498f, 27.074f, 2.37f, 0.97f }, /* 66 Izar */
    { 11.0307f, 56.382f, 2.37f, -0.02f }, /* 67 Merak */
    { 21.7364f, 9.875f, 2.39f, 1.52f }, /* 68 Enif */
    { 0.4381f, -42.306f, 2.40f, 1.09f }, /* 69 Ankaa */
    { 17.7081f, -39.030f, 2.41f, -0.17f }, /* 70 KappaSco */
    { 23.0629f, 28.083f, 2.42f, 1.67f }, /* 71 Scheat */
    { 17.1730f, -15.725f, 2.43f, 0.06f }, /* 72 Sabik */
    { 11.8972f, 53.695f, 2.44f, 0.04f }, /* 73 Phecda */
    { 7.4016f, -29.303f, 2.45f, -0.08f }, /* 74 Aludra */
    { 21.3097f, 62.585f, 2.45f, 0.22f }, /* 75 Alderamin */
    { 0.9451f, 60.717f, 2.47f, -0.15f }, /* 76 GammaCas */
    { 20.7702f, 33.970f, 2.48f, 1.03f }, /* 77 GienahCyg */
    { 23.0794f, 15.205f, 2.49f, -0.04f }, /* 78 Markab */
    { 3.0380f, 4.090f, 2.54f, 1.64f }, /* 79 Menkar */
    { 16.6193f, -10.567f, 2.54f, 0.02f }, /* 80 ZetaOph */
    { 11.2351f, 20.524f, 2.56f, 0.12f }, /* 81 Zosma */
    { 5.5455f, -17.822f, 2.58f, 0.21f }, /* 82 Arneb */
    { 12.2634f, -17.542f, 2.59f, -0.11f }, /* 83 GienahCrv */
    { 19.0435f, -29.880f, 2.60f, 0.08f }, /* 84 Ascella */
    { 15.2834f, -9.383f, 2.61f, -0.11f }, /* 85 Zubeneschamali */
    { 16.0906f, -19.806f, 2.62f, -0.07f }, /* 86 Acrab */
    { 5.9954f, 37.213f, 2.62f, -0.08f }, /* 87 ThetaAur */
    { 15.7378f, 6.426f, 2.63f, 1.17f }, /* 88 Unukalhai */
    { 1.9107f, 20.808f, 2.64f, 0.13f }, /* 89 Sheratan */
    { 5.6609f, -34.074f, 2.65f, -0.12f }, /* 90 Phact */
    { 12.5731f, -23.397f, 2.65f, 0.89f }, /* 91 Kraz */
    { 13.9114f, 18.398f, 2.68f, 0.58f }, /* 92 Muphrid */
    { 1.4303f, 60.235f, 2.68f, 0.13f }, /* 93 Ruchbah */
    { 17.5127f, -37.296f, 2.69f, -0.18f }, /* 94 Lesath */
    { 4.9499f, 33.166f, 2.69f, 1.53f }, /* 95 IotaAur */
    { 18.3499f, -29.828f, 2.70f, 1.38f }, /* 96 KausMedia */
    { 7.2859f, -37.097f, 2.70f, 1.62f }, /* 97 PiPup */
    { 19.7710f, 10.613f, 2.72f, 1.52f }, /* 98 Tarazed */
    { 16.3999f, 61.514f, 2.74f, 0.91f }, /* 99 EtaDra */
    { 12.6943f, -1.449f, 2.74f, 0.36f }, /* 100 Porrima */
    { 16.2391f, -3.694f, 2.74f, 1.58f }, /* 101 DeltaOph */
    { 14.8480f, -16.042f, 2.75f, 0.15f }, /* 102 Zubenelgenubi */
    { 16.5037f, 21.490f, 2.77f, 0.94f }, /* 103 Kornephoros */
    { 17.7245f, 4.567f, 2.77f, 1.16f }, /* 104 Cebalrai */
    { 5.1308f, -5.086f, 2.79f, 0.13f }, /* 105 Cursa */
    { 17.5072f, 52.301f, 2.79f, 0.98f }, /* 106 Rastaban */
    { 18.4662f, -25.422f, 2.81f, 1.04f }, /* 107 KausBorealis */
    { 16.6881f, 31.603f, 2.81f, 0.65f }, /* 108 ZetaHer */
    { 8.1258f, -24.304f, 2.81f, 0.43f }, /* 109 RhoPup */
    { 16.5980f, -28.216f, 2.82f, -0.25f }, /* 110 TauSco */
    { 0.2206f, 15.184f, 2.83f, -0.23f }, /* 111 Algenib */
    { 13.0363f, 10.959f, 2.85f, 0.94f }, /* 112 Vindemiatrix */
    { 21.7840f, -16.127f, 2.85f, 0.29f }, /* 113 DenebAlgedi */
    { 3.9022f, 31.884f, 2.85f, 0.12f }, /* 114 ZetaPer */
    { 5.4708f, -20.759f, 2.84f, 0.82f }, /* 115 Nihal */
    { 6.3827f, 22.514f, 2.87f, 1.64f }, /* 116 MuGem */
    { 21.5260f, -5.571f, 2.87f, 0.83f }, /* 117 Sadalsuud */
    { 3.7914f, 24.105f, 2.87f, -0.09f }, /* 118 Alcyone */
    { 19.7496f, 45.131f, 2.87f, -0.03f }, /* 119 DeltaCyg */
    { 3.9642f, 40.010f, 2.89f, -0.18f }, /* 120 EpsPer */
    { 16.3531f, -25.593f, 2.89f, 0.13f }, /* 121 SigmaSco */
    { 15.9809f, -26.114f, 2.89f, -0.19f }, /* 122 PiSco */
    { 3.0799f, 53.506f, 2.93f, 0.70f }, /* 123 GammaPer */
    { 22.7167f, 30.221f, 2.94f, 0.86f }, /* 124 Matar */
    { 3.9672f, -13.509f, 2.95f, 1.59f }, /* 125 Zaurak */
    { 12.4977f, -16.515f, 2.95f, -0.05f }, /* 126 Algorab */
    { 22.0964f, -0.320f, 2.95f, 0.98f }, /* 127 Sadalmelik */
    { 9.7642f, 23.774f, 2.98f, 0.81f }, /* 128 EpsLeo */
    { 12.1688f, -22.620f, 3.00f, 1.33f }, /* 129 EpsCrv */
    { 2.1590f, 34.987f, 3.00f, 0.14f }, /* 130 BetaTri */
    { 19.0902f, 13.863f, 2.99f, 0.01f }, /* 131 ZetaAql */
    { 18.0968f, -30.424f, 2.99f, 1.00f }, /* 132 Alnasl */
    { 5.0328f, 43.823f, 2.99f, 0.54f }, /* 133 EpsAur */
    { 3.7154f, 47.788f, 3.01f, -0.13f }, /* 134 DeltaPer */
    { 21.8988f, -37.365f, 3.01f, -0.12f }, /* 135 GammaGru */
    { 11.1610f, 44.498f, 3.01f, 1.14f }, /* 136 PsiUMa */
    { 7.3354f, 21.982f, 3.53f, 0.34f }, /* 137 DeltaGem */
    { 6.7322f, 25.131f, 3.06f, 1.40f }, /* 138 Mebsuta */
    { 14.5347f, 38.308f, 3.03f, 0.19f }, /* 139 Seginus */
    { 15.3455f, 71.834f, 3.05f, 0.05f }, /* 140 Pherkad */
    { 20.3502f, -14.781f, 3.05f, 0.79f }, /* 141 Dabih */
    { 10.3721f, 41.499f, 3.05f, 1.59f }, /* 142 MuUMa */
    { 19.2093f, 67.662f, 3.07f, 1.00f }, /* 143 Altais */
    { 19.5120f, 27.960f, 3.08f, 1.13f }, /* 144 Albireo */
    { 8.9232f, 5.946f, 3.11f, 1.00f }, /* 145 ZetaHya */
    { 9.3509f, 34.393f, 3.13f, 1.55f }, /* 146 AlphaLyn */
    { 8.9868f, 48.042f, 3.14f, 0.19f }, /* 147 IotaUMa */
    { 17.2505f, 24.839f, 3.14f, 0.08f }, /* 148 DeltaHer */
    { 17.2508f, 36.809f, 3.16f, 1.44f }, /* 149 PiHer */
    { 9.5476f, 51.678f, 3.17f, 0.46f }, /* 150 ThetaUMa */
    { 17.1464f, 65.715f, 3.17f, -0.12f }, /* 151 ZetaDra */
    { 5.1086f, 41.234f, 3.17f, -0.18f }, /* 152 EtaAur */
    { 18.7609f, -26.991f, 3.17f, -0.11f }, /* 153 PhiSgr */
    { 16.9612f, 9.375f, 3.20f, 1.15f }, /* 154 KappaOph */
    { 21.2156f, 30.227f, 3.21f, 0.99f }, /* 155 ZetaCyg */
    { 23.6558f, 77.632f, 3.21f, 1.03f }, /* 156 Errai */
    { 21.4777f, 70.561f, 3.23f, -0.22f }, /* 157 Alfirk */
    { 20.1884f, -0.822f, 3.23f, -0.07f }, /* 158 ThetaAql */
    { 16.3053f, -4.692f, 3.24f, 0.96f }, /* 159 EpsOph */
    { 18.9824f, 32.690f, 3.25f, -0.05f }, /* 160 Sulafat */
    { 0.6555f, 30.861f, 3.27f, 1.28f }, /* 161 DeltaAnd */
    { 22.9108f, -15.821f, 3.27f, 0.05f }, /* 162 Skat */
    { 15.4155f, 58.966f, 3.29f, 1.16f }, /* 163 IotaDra */
    { 12.2571f, 57.033f, 3.31f, 0.08f }, /* 164 Megrez */
    { 19.1157f, -27.671f, 3.32f, 1.19f }, /* 165 TauSgr */
    { 11.2373f, 15.430f, 3.33f, -0.01f }, /* 166 ThetaLeo */
    { 22.1809f, 58.201f, 3.35f, 1.57f }, /* 167 ZetaCep */
    { 19.4249f, 3.115f, 3.36f, 0.32f }, /* 168 DeltaAql */
    { 8.5044f, 60.718f, 3.36f, 0.85f }, /* 169 OmicronUMa */
    { 13.5783f, -0.596f, 3.37f, 0.11f }, /* 170 ZetaVir */
    { 1.9066f, 63.670f, 3.37f, -0.15f }, /* 171 Segin */
    { 5.5856f, 9.934f, 3.39f, -0.16f }, /* 172 Meissa */
    { 22.6910f, 10.831f, 3.40f, -0.09f }, /* 173 Homam */
    { 20.7547f, 61.839f, 3.43f, 0.92f }, /* 174 EtaCep */
    { 10.2782f, 23.417f, 3.44f, 0.31f }, /* 175 ZetaLeo */
    { 19.1041f, -4.882f, 3.44f, -0.09f }, /* 176 LambdaAql */
    { 1.1431f, -10.182f, 3.45f, 1.16f }, /* 177 EtaCet */
    { 10.2849f, 42.915f, 3.45f, 0.03f }, /* 178 LambdaUMa */
    { 2.7217f, 3.236f, 3.47f, 0.09f }, /* 179 Kaffaljidhma */
    { 15.2584f, 33.315f, 3.47f, 0.95f }, /* 180 DeltaBoo */
    { 16.7149f, 38.922f, 3.48f, 0.92f }, /* 181 EtaHer */
    { 11.3080f, 33.094f, 3.48f, 1.40f }, /* 182 NuUMa */
    { 1.7344f, -15.937f, 3.50f, 0.72f }, /* 183 TauCet */
    { 15.0324f, 40.390f, 3.50f, 0.96f }, /* 184 Nekkar */
    { 18.8347f, 33.363f, 3.52f, 0.00f }, /* 185 Sheliak */
    { 10.1222f, 16.763f, 3.52f, -0.03f }, /* 186 EtaLeo */
    { 22.8280f, 66.201f, 3.52f, 1.05f }, /* 187 IotaCep */
    { 4.4769f, 19.180f, 3.53f, 1.01f }, /* 188 EpsTau */
    { 0.3238f, -8.824f, 3.56f, 1.22f }, /* 189 IotaCet */
    { 20.3010f, -12.545f, 3.57f, 0.88f }, /* 190 Algedi */
    { 1.4004f, -8.183f, 3.60f, 1.06f }, /* 191 ThetaCet */
    { 9.0605f, 47.157f, 3.60f, 0.00f }, /* 192 KappaUMa */
    { 11.8448f, 1.765f, 3.61f, 0.55f }, /* 193 BetaVir */
    { 20.6258f, 14.595f, 3.63f, 0.44f }, /* 194 Rotanev */
    { 2.3224f, -2.977f, 3.50f, 1.42f }, /* 195 Mira */
    { 22.4806f, -0.020f, 3.65f, 0.38f }, /* 196 ZetaAqr */
    { 14.0731f, 64.376f, 3.65f, -0.05f }, /* 197 Thuban */
    { 4.3297f, 15.628f, 3.65f, 0.99f }, /* 198 GammaTau */
    { 23.1574f, -21.172f, 3.66f, 1.24f }, /* 199 Aqr88 */
    { 9.5254f, 63.062f, 3.65f, 0.33f }, /* 200 UMa23 */
    { 21.6682f, -16.662f, 3.67f, 0.32f }, /* 201 Nashira */
    { 19.9219f, 6.407f, 3.71f, 0.86f }, /* 202 Alshain */
    { 11.7675f, 47.779f, 3.71f, 1.18f }, /* 203 ChiUMa */
    { 22.8770f, -7.580f, 3.73f, 1.64f }, /* 204 LambdaAqr */
    { 1.8577f, -10.335f, 3.73f, 1.14f }, /* 205 BatenKaitos */
    { 21.4441f, -22.411f, 3.74f, 1.00f }, /* 206 ZetaCap */
    { 17.8921f, 56.873f, 3.75f, 1.18f }, /* 207 Grumium */
    { 2.8450f, 55.896f, 3.76f, 1.68f }, /* 208 EtaPer */
    { 20.6606f, 15.912f, 3.77f, -0.06f }, /* 209 Sualocin */
    { 20.7946f, -9.496f, 3.77f, 0.00f }, /* 210 Albali */
    { 9.8494f, 59.039f, 3.80f, 0.28f }, /* 211 UpsUMa */
    { 2.0340f, 2.764f, 3.82f, 0.03f }, /* 212 Alrescha */
    { 22.3609f, -1.387f, 3.84f, -0.05f }, /* 213 Sadachbia */
    { 11.5232f, 69.331f, 3.84f, 1.62f }, /* 214 LambdaDra */
    { 19.8028f, 70.268f, 3.83f, 0.89f }, /* 215 EpsDra */
    { 12.5580f, 69.788f, 3.87f, -0.13f }, /* 216 KappaDra */
    { 0.9458f, 38.499f, 3.87f, 0.13f }, /* 217 MuAnd */
    { 9.8794f, 26.007f, 3.88f, 1.22f }, /* 218 MuLeo */
    { 23.3826f, -20.100f, 3.97f, 1.10f }, /* 219 Aqr98 */
    { 2.0001f, -21.078f, 4.00f, 1.57f }, /* 220 UpsCet */
    { 5.0570f, 60.442f, 4.03f, 0.92f }, /* 221 BetaCam */
    { 22.5893f, -0.117f, 4.04f, -0.09f }, /* 222 EtaAqr */
    { 2.6580f, 0.329f, 4.07f, -0.22f }, /* 223 DeltaCet */
    { 22.4862f, 58.415f, 4.07f, 0.66f }, /* 224 DeltaCep */
    { 22.2808f, -7.783f, 4.16f, 0.98f }, /* 225 AnchaAqr */
    { 22.6776f, -27.044f, 4.17f, -0.11f }, /* 226 EpsPsA */
    { 16.7662f, 82.037f, 4.21f, 0.89f }, /* 227 EpsUMi */
    { 2.7490f, 10.114f, 4.27f, 0.31f }, /* 228 MuCet */
    { 2.4693f, 8.460f, 4.28f, -0.05f }, /* 229 Xi2Cet */
    { 4.9008f, 66.343f, 4.29f, 0.03f }, /* 230 AlphaCam */
    { 15.7343f, 77.795f, 4.29f, 0.04f }, /* 231 ZetaUMi */
    { 0.9768f, -29.357f, 4.31f, -0.16f }, /* 232 AlphaScl */
    { 17.5369f, 86.586f, 4.35f, 0.02f }, /* 233 Yildun */
    { 4.9548f, 53.752f, 4.47f, -0.02f }, /* 234 Cam7 */
    { 3.8393f, 71.332f, 4.63f, 0.03f }, /* 235 GammaCam */
    { 16.2918f, 75.755f, 4.95f, 0.37f }  /* 236 EtaUMi */
};

/* A run is a polyline of n stars: SKY_FIG_IDX[first .. first + n - 1]. */
typedef struct { uint16_t first; uint8_t n; uint8_t fig; } sky_fig_run_t;

static const sky_fig_run_t SKY_FIG_RUNS[SKY_FIG_RUN_N] = {
    { 0, 8, 0 }, /* Big Dipper */
    { 8, 8, 1 }, /* Little Dipper */
    { 16, 5, 2 }, /* Cassiopeia */
    { 21, 7, 3 }, /* Cepheus */
    { 28, 2, 3 }, /* Cepheus */
    { 30, 4, 4 }, /* Giraffe */
    { 34, 5, 5 }, /* Pegasus */
    { 39, 3, 5 }, /* Pegasus */
    { 42, 2, 5 }, /* Pegasus */
    { 44, 4, 6 }, /* Andromeda */
    { 48, 2, 6 }, /* Andromeda */
    { 50, 5, 7 }, /* Perseus */
    { 55, 2, 7 }, /* Perseus */
    { 57, 6, 8 }, /* Water Bearer */
    { 63, 6, 8 }, /* Water Bearer */
    { 69, 6, 9 }, /* Sea Goat */
    { 75, 9, 10 }, /* Whale */
    { 84, 3, 10 }, /* Whale */
    { 87, 4, 10 }, /* Whale */
    { 91, 8, 11 }, /* Orion */
    { 99, 2, 11 }, /* Orion */
    { 101, 2, 11 }, /* Orion */
    { 103, 2, 11 }, /* Orion */
    { 105, 3, 12 }, /* Twins */
    { 108, 3, 12 }, /* Twins */
    { 111, 7, 13 }, /* Charioteer */
    { 118, 3, 14 }, /* Swan */
    { 121, 4, 14 }, /* Swan */
    { 125, 6, 15 }, /* Lion */
    { 131, 5, 15 }, /* Lion */
    { 136, 2, 15 }  /* Lion */
};

static const uint8_t SKY_FIG_IDX[138] = {
    35, 59, 30, 164, 73, 67, 31, 164, 39, 233, 227, 231, 50, 140, 236, 231,
    63, 60, 76, 93, 171, 75, 157, 156, 187, 224, 167, 75, 157, 187, 234, 221,
    230, 235, 48, 71, 78, 111, 48, 78, 173, 68, 71, 124, 48, 161, 47, 52,
    47, 217, 123, 32, 134, 120, 114, 32, 53, 210, 117, 127, 213, 196, 222, 127,
    225, 204, 162, 199, 219, 190, 141, 206, 113, 201, 190, 79, 179, 223, 195, 205,
    191, 177, 44, 189, 205, 183, 44, 79, 228, 229, 179, 9, 25, 56, 27, 29,
    51, 6, 56, 9, 29, 172, 9, 172, 25, 22, 138, 116, 16, 137, 38, 5,
    37, 87, 26, 95, 133, 5, 18, 58, 144, 119, 58, 77, 155, 20, 186, 43,
    175, 218, 128, 43, 81, 54, 166, 20, 81, 166
};

/** Colour index 0 (blue white) to 4 (orange) from B-V, breaks 0, 0.35, 0.8, 1.3. */
static inline int sky_star_tint(float bv)
{
    if (bv < 0.0f) {
        return 0;
    }
    if (bv < 0.35f) {
        return 1;
    }
    if (bv < 0.8f) {
        return 2;
    }
    if (bv < 1.3f) {
        return 3;
    }
    return 4;
}

typedef struct { float m[3][3]; } sky_m3_t;

/** Equatorial unit vector (x to RA 0 h, z to the pole). */
static inline sky_v3_t sky_eq_vec(float ra_h, float dec_deg)
{
    const float r = ra_h * 15.0f * SKY_DEG2RAD;
    const float d = dec_deg * SKY_DEG2RAD;
    const float cd = cosf(d);
    return sky_v3(cd * cosf(r), cd * sinf(r), sinf(d));
}

/** Rotation from an equatorial vector to the world frame (x east, y north,
 *  z up) at local sidereal time lst_h and latitude lat_deg. Equals
 *  sky_hadec_to_vec(sky_ha_deg(lst, ra), dec, lat) applied to sky_eq_vec. */
static inline sky_m3_t sky_eq_to_world(float lst_h, float lat_deg)
{
    const float l = lst_h * 15.0f * SKY_DEG2RAD;
    const float p = lat_deg * SKY_DEG2RAD;
    const float sl = sinf(l), cl = cosf(l), sp = sinf(p), cp = cosf(p);
    sky_m3_t r = { {
        { -sl, cl, 0.0f },
        { -sp * cl, -sp * sl, cp },
        { cp * cl, cp * sl, sp }
    } };
    return r;
}

static inline sky_v3_t sky_m3_apply(const sky_m3_t *m, sky_v3_t v)
{
    return sky_v3(m->m[0][0] * v.x + m->m[0][1] * v.y + m->m[0][2] * v.z,
                  m->m[1][0] * v.x + m->m[1][1] * v.y + m->m[1][2] * v.z,
                  m->m[2][0] * v.x + m->m[2][1] * v.y + m->m[2][2] * v.z);
}

/** Angular separation of two equatorial positions, degrees. */
static inline float sky_radec_sep_deg(float ra1_h, float dec1_deg, float ra2_h, float dec2_deg)
{
    return sky_sep_deg(sky_eq_vec(ra1_h, dec1_deg), sky_eq_vec(ra2_h, dec2_deg));
}

/** Mean direction of the stars of one figure (all its runs). */
static inline void sky_fig_centre(int fig, float *ra_h, float *dec_deg)
{
    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    for (int i = 0; i < SKY_FIG_RUN_N; i++) {
        if (SKY_FIG_RUNS[i].fig != fig) {
            continue;
        }
        for (int j = 0; j < SKY_FIG_RUNS[i].n; j++) {
            const sky_star_t *s = &SKY_STARS[SKY_FIG_IDX[SKY_FIG_RUNS[i].first + j]];
            const sky_v3_t v = sky_eq_vec(s->ra_h, s->dec_deg);
            sx += v.x;
            sy += v.y;
            sz += v.z;
        }
    }
    if (ra_h) {
        *ra_h = sky_wrap24(atan2f(sy, sx) * SKY_RAD2DEG / 15.0f);
    }
    if (dec_deg) {
        *dec_deg = asinf(sz / sqrtf(sx * sx + sy * sy + sz * sz)) * SKY_RAD2DEG;
    }
}
