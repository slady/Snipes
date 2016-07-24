#include <time.h>
#include <stdio.h>
#include <wchar.h>
#include "config.h"
#include "Snipes.h"
#include "types.h"
#include "macros.h"
#include "console.h"
#include "timer.h"
#include "sound.h"
#include "keyboard.h"

bool got_ctrl_break = false;
bool forfeit_match = false;
bool sound_enabled = true;
bool shooting_sound_enabled = false;
BYTE fast_forward = false;
BYTE spacebar_state = false;
static BYTE keyboard_state = 0;

static WORD random_seed_lo = 33, random_seed_hi = 467;
WORD GetRandomMasked(WORD mask)
{
	random_seed_lo *= 2;
	if (random_seed_lo  > 941)
		random_seed_lo -= 941;
	random_seed_hi *= 2;
	if (random_seed_hi  > 947)
		random_seed_hi -= 947;
	return (random_seed_lo + random_seed_hi) & mask;
}
template <WORD RANGE>
WORD GetRandomRanged()
{
	WORD mask = RANGE-1;
	mask |= mask >> 1;
	mask |= mask >> 2;
	mask |= mask >> 4;
	mask |= mask >> 8;
	if (mask == RANGE-1)
		return GetRandomMasked(mask);
	for (;;)
	{
		WORD number = GetRandomMasked(mask);
		if (number < RANGE)
			return number;
	}
}

Uint skillLevelLetter = 0;
Uint skillLevelNumber = 1;

void ParseSkillLevel(char *skillLevel, DWORD skillLevelLength)
{
	Uint skillLevelNumberTmp = 0;
	for (; skillLevelLength; skillLevel++, skillLevelLength--)
	{
		if (skillLevelNumberTmp >= 0x80)
		{
			// strange behavior, but this is what the original game does
			skillLevelNumber = 1;
			return;
		}
		char ch = *skillLevel;
		if (inrange(ch, 'a', 'z'))
			skillLevelLetter = ch - 'a';
		else
		if (inrange(ch, 'A', 'Z'))
			skillLevelLetter = ch - 'A';
		else
		if (inrange(ch, '0', '9'))
			skillLevelNumberTmp = skillLevelNumberTmp * 10 + (ch - '0');
	}
	if (inrange(skillLevelNumberTmp, 1, 9))
		skillLevelNumber = skillLevelNumberTmp;
}

void ReadSkillLevel()
{
	char skillLevel[0x80] = {};
	DWORD skillLevelLength = ReadTextFromConsole(skillLevel, _countof(skillLevel));

	if (skillLevel[skillLevelLength-1] == '\n')
		skillLevelLength--;
	if (skillLevel[skillLevelLength-1] == '\r')
		skillLevelLength--;

	for (Uint i=0; i<skillLevelLength; i++)
		if (skillLevel[i] != ' ')
		{
			ParseSkillLevel(skillLevel + i, skillLevelLength - i);
			break;
		}
}

static BYTE snipeShootingAccuracyTable['Z'-'A'+1] = {2, 3, 4, 3, 4, 4, 3, 4, 3, 4, 4, 5, 3, 4, 3, 4, 3, 4, 3, 4, 4, 5, 4, 4, 5, 5};
static bool enableGhostSnipesTable    ['Z'-'A'+1] = {0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1};
static bool rubberBulletTable         ['Z'-'A'+1] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
static BYTE ghostBitingAccuracyTable  ['Z'-'A'+1] = {0x7F, 0x7F, 0x7F, 0x3F, 0x3F, 0x1F, 0x7F, 0x7F, 0x3F, 0x3F, 0x1F, 0x1F, 0x7F, 0x7F, 0x3F, 0x3F, 0x7F, 0x7F, 0x3F, 0x3F, 0x1F, 0x1F, 0x3F, 0x1F, 0x1F, 0x0F};
static BYTE maxSnipesTable            ['9'-'1'+1] = { 10,  20,  30,  40,  60,  80, 100, 120, 150};
static BYTE numGeneratorsTable        ['9'-'1'+1] = {  3,   3,   4,   4,   5,   5,   6,   8,  10};
static BYTE numLivesTable             ['9'-'1'+1] = {  5,   5,   5,   5,   5,   4,   4,   3,   2};

bool enableElectricWalls, enableGhostSnipes, generatorsResistSnipeBullets, enableRubberBullets;
BYTE snipeShootingAccuracy, ghostBitingAccuracy, maxSnipes, numGeneratorsAtStart, numLives;

enum MoveDirection : BYTE  // progresses from 0 to 7 in the clockwise direction
{
	MoveDirection_Up,
	MoveDirection_UpRight,
	MoveDirection_Right,
	MoveDirection_DownRight,
	MoveDirection_Down,
	MoveDirection_DownLeft,
	MoveDirection_Left,
	MoveDirection_UpLeft,
	MoveDirection_COUNT
};
enum MoveDirectionMask : BYTE
{
	MoveDirectionMask_Diagonal = 1,
	MoveDirectionMask_UpDown   = 4,
	MoveDirectionMask_All      = MoveDirection_COUNT - 1
};
MoveDirection  operator+ (MoveDirection  dir, int n                 ) {return (MoveDirection ) ((BYTE )dir +  n);   }
MoveDirection  operator- (MoveDirection  dir, int n                 ) {return (MoveDirection ) ((BYTE )dir -  n);   }
MoveDirection  operator& (MoveDirection  dir, MoveDirectionMask mask) {return (MoveDirection ) ((BYTE )dir &  mask);}
MoveDirection &operator+=(MoveDirection &dir, int n                 ) {return (MoveDirection&) ((BYTE&)dir += n);   }
MoveDirection &operator-=(MoveDirection &dir, int n                 ) {return (MoveDirection&) ((BYTE&)dir -= n);   }
MoveDirection &operator&=(MoveDirection &dir, MoveDirectionMask mask) {return (MoveDirection&) ((BYTE&)dir &= mask);}
MoveDirection &operator++(MoveDirection &dir                        ) {return (MoveDirection&)++(BYTE&)dir;         }
MoveDirection &operator--(MoveDirection &dir                        ) {return (MoveDirection&)--(BYTE&)dir;         }
MoveDirection  operator++(MoveDirection &dir, int                   ) {return (MoveDirection ) ((BYTE&)dir)++;      }
MoveDirection  operator--(MoveDirection &dir, int                   ) {return (MoveDirection ) ((BYTE&)dir)--;      }

enum OrthogonalDirection : BYTE
{
	OrthogonalDirection_Up,
	OrthogonalDirection_Right,
	OrthogonalDirection_Down,
	OrthogonalDirection_Left,
	OrthogonalDirection_COUNT
};
enum OrthogonalDirectionMask : BYTE
{
	OrthogonalDirectionMask_All = OrthogonalDirection_COUNT - 1
};
OrthogonalDirection  operator+ (OrthogonalDirection  dir, int n                       ) {return (OrthogonalDirection ) ((BYTE )dir +  n);   }
OrthogonalDirection  operator& (OrthogonalDirection  dir, OrthogonalDirectionMask mask) {return (OrthogonalDirection ) ((BYTE )dir &  mask);}
MoveDirection OrthoDirectionToMoveDirection(OrthogonalDirection dir) {return (MoveDirection)(dir << 1);}

static const BYTE bulletBounceTable[OrthogonalDirection_COUNT][MoveDirection_COUNT] =
{
	{4, 3, 4, 4, 4, 4, 4, 5,},
	{6, 7, 6, 5, 6, 6, 6, 6,},
	{0, 0, 0, 1, 0, 7, 0, 0,},
	{2, 2, 2, 2, 2, 3, 2, 1,},
};

struct Object
{
	BYTE next; // objects[] index of the next object in the linked list of this object's type
	BYTE generalPurpose1;
	union
	{
		struct {BYTE x, y;};
		WORD xy;
	};
	BYTE generalPurpose2;
	BYTE generalPurpose3;
#ifdef OBJECT_TABLE_BINARY_COMPATIBILITY
	WORD sprite; // FAKE_POINTER to current sprite frame
#else
	const WORD *sprite;
#endif
};

#define DEFINE_OBJECT_MEMBER(type,name,generalPurposeNum,fakeObjectOffset) \
	struct __##name\
	{\
		operator         type &() const {return (type&)((Object*)(this-fakeObjectOffset)-1)->generalPurposeNum;}\
		type &operator =(type n)        {return (type&)((Object*)(this-fakeObjectOffset)-1)->generalPurposeNum =       n;}\
		type &operator =(int  n)        {return (type&)((Object*)(this-fakeObjectOffset)-1)->generalPurposeNum = (type)n;}\
	} name
#define DEFINE_OBJECT_AND_MEMBERS(className,type1,member1,type2,member2,type3,member3) \
	struct className : public Object\
	{\
		DEFINE_OBJECT_MEMBER(type1,member1,generalPurpose1,0);\
		DEFINE_OBJECT_MEMBER(type2,member2,generalPurpose2,1);\
		DEFINE_OBJECT_MEMBER(type3,member3,generalPurpose3,2);\
		className() {__debugbreak();}\
	}
DEFINE_OBJECT_AND_MEMBERS(Generator,    BYTE, unused,   BYTE,          spawnFrame,    BYTE, animFrame);
DEFINE_OBJECT_AND_MEMBERS(Explosion,    BYTE, unused,   BYTE,          spriteSize,    BYTE, animFrame);
DEFINE_OBJECT_AND_MEMBERS(MovingObject, BYTE, general1, MoveDirection, moveDirection, BYTE, general2 );

#define DEFINE_MOVING_OBJECT_MEMBER(type,name,generalNum,fakeObjectOffset) \
	struct __##name \
	{\
		operator         type &() const {return (type&)((MovingObject*)(this-fakeObjectOffset)-1)->generalNum;}\
		type &operator =(type n)        {return (type&)((MovingObject*)(this-fakeObjectOffset)-1)->generalNum = n;}\
		type &operator =(int  n)        {return (type&)((MovingObject*)(this-fakeObjectOffset)-1)->generalNum = n;}\
	} name
#define DEFINE_MOVING_OBJECT_AND_MEMBERS(className,type1,member1,type2,member2) \
	struct className : public MovingObject\
	{\
		DEFINE_MOVING_OBJECT_MEMBER(type1,member1,general1,0);\
		DEFINE_MOVING_OBJECT_MEMBER(type2,member2,general2,1);\
		className() {__debugbreak();}\
	}
DEFINE_MOVING_OBJECT_AND_MEMBERS(Player, BYTE, firingFrame,   BYTE, inputFrame);
DEFINE_MOVING_OBJECT_AND_MEMBERS(Enemy,  BYTE, movementFlags, BYTE, moveFrame );
DEFINE_MOVING_OBJECT_AND_MEMBERS(Bullet, BYTE, bulletType,    BYTE, animFrame );

struct Snipe : public Enemy {};
struct Ghost : public Enemy {};

enum EnemyMovementFlag : BYTE
{
	EnemyMovementFlag_TurnDirection     = 1 << 0, // clear = clockwise, set = counterclockwise; set in stone when a snipe comes out of a generator
	EnemyMovementFlag_GhostMoveStraight = 1 << 1, // for ghosts only; clear = move based on direction of player, set = keep moving in current direction until an obstacle is hit
};

enum SoundEffect : BYTE
{
	SoundEffect_PlayerBullet  = 0,
	SoundEffect_SnipeBullet   = 1,
	SoundEffect_ExplodeGhost  = 2,
	SoundEffect_ExplodeSnipe  = 3,
	SoundEffect_ExplodePlayer = 4,
	SoundEffect_None = 0xFF
};

BYTE playerFiringPeriod; // in pairs of frames
WORD frame;
static bool playerAnimFrame, isPlayerDying, isPlayerExploding, data_CBF;
static BYTE lastHUD_numGhosts, lastHUD_numGeneratorsKilled, lastHUD_numSnipes, lastHUD_numPlayerDeaths, objectHead_free, objectHead_bullets, objectHead_explosions, objectHead_ghosts, objectHead_snipes, objectHead_generators, numGenerators, numGhosts, numBullets, numPlayerDeaths, numSnipes, data_C74, orthoDistance, data_C77, data_C78;
static OrthogonalDirection bulletCollisionDirection;
static SoundEffect currentSoundEffect = SoundEffect_None;
static BYTE currentSoundEffectFrame;
static WORD lastHUD_numGhostsKilled, lastHUD_numSnipesKilled, numGhostsKilled, numSnipesKilled, viewportFocusX, viewportFocusY;
static MazeTile *bulletTestPos;
static SHORT lastHUD_score, score;
Object *currentObject;
const WORD *currentSprite;

#define MAX_OBJECTS 0x100

static BYTE bulletLifetime[MAX_OBJECTS];

static Object objects[MAX_OBJECTS];

#define OBJECT_PLAYER          0x00
#define OBJECT_LASTFREE        0xFD
#define OBJECT_PLAYEREXPLOSION 0xFE

enum
{
	BulletType_Player = 0,
	BulletType_Snipe  = 6,
};

Player    &player          = (Player   &)objects[OBJECT_PLAYER         ];
Explosion &playerExplosion = (Explosion&)objects[OBJECT_PLAYEREXPLOSION];

static MazeTile maze[MAZE_WIDTH * MAZE_HEIGHT];

static const char statusLine[] = "\xDA\xBF\xB3\x01\x1A\xB3\x02\xB3""Skill""\xC0\xD9\xB3\x01\x1A\xB3\x02\xB3""Time  Men Left                 Score     0  0000001 Man Left""e";

void OutputHUD()
{
	char (&scratchBuffer)[WINDOW_WIDTH] = (char(&)[WINDOW_WIDTH])maze;

	char skillLetter = skillLevelLetter + 'A';
	memset(scratchBuffer, ' ', WINDOW_WIDTH);
	outputText  (0x17, WINDOW_WIDTH, 0, 0, scratchBuffer);
	outputText  (0x17, WINDOW_WIDTH, 1, 0, scratchBuffer);
	outputText  (0x17, WINDOW_WIDTH, 2, 0, scratchBuffer);
	outputText  (0x17,     2, 0,  0, statusLine);
	outputNumber(0x13,  0, 2, 0,  3, 0);
	outputText  (0x17,     1, 0,  6, statusLine+2);
	outputText  (0x13,     2, 0,  8, statusLine+3);
	outputNumber(0x13,  0, 4, 0, 11, 0);
	outputText  (0x17,     1, 0, 16, statusLine+5);
	outputText  (0x13,     1, 0, 18, statusLine+6);
	outputNumber(0x13,  0, 4, 0, 20, 0);
	outputText  (0x17,     1, 0, 25, statusLine+7);
	outputText  (0x17,     5, 0, 27, statusLine+8);
	outputNumber(0x17,  0, 1, 0, 38, skillLevelNumber);
	outputText  (0x17,     1, 0, 37, &skillLetter);
	outputText  (0x17,     2, 1,  0, statusLine+13);
	outputText  (0x17,     1, 1,  6, statusLine+15);
	outputText  (0x17,     2, 1,  8, statusLine+16);
	outputText  (0x17,     1, 1, 16, statusLine+18);
	outputText  (0x17,     1, 1, 18, statusLine+19);
	outputText  (0x17,     1, 1, 25, statusLine+20);
	outputText  (0x17,     4, 1, 27, statusLine+21);
	memcpy(scratchBuffer, statusLine+25, 40);
	scratchBuffer[0] = numLives + '0';
	scratchBuffer[1] = 0;
	if (numLives == 1)
		scratchBuffer[6] = 'a';
	outputText  (0x17, 40, 2,  0, scratchBuffer);

	lastHUD_numGhosts = 0xFF;
	lastHUD_numGeneratorsKilled = 0xFF;
	lastHUD_numSnipes = 0xFF;
	lastHUD_numGhostsKilled = 0xFFFF;
	lastHUD_numSnipesKilled = 0xFFFF;
	lastHUD_numPlayerDeaths = 0xFF;
	lastHUD_score = -1;
}

#define MAZE_SCRATCH_BUFFER_SIZE 320

void CreateMaze_helper(SHORT &data_1E0, BYTE &data_2AF)
{
	switch (data_2AF)
	{
	case 0:
		data_1E0 -= 0x10;
		if (data_1E0 < 0)
			data_1E0 += MAZE_SCRATCH_BUFFER_SIZE;
		break;
	case 1:
		data_1E0 += 0x10;
		if (data_1E0 >= MAZE_SCRATCH_BUFFER_SIZE)
			data_1E0 -= MAZE_SCRATCH_BUFFER_SIZE;
		break;
	case 2:
		if ((data_1E0 & 0xF) == 0)
			data_1E0 += 0xF;
		else
			data_1E0--;
		break;
	case 3:
		if ((data_1E0 & 0xF) == 0xF)
			data_1E0 -= 0xF;
		else
			data_1E0++;
		break;
	default:
		__assume(0);
	}
}

void CreateMaze()
{
	static WORD data_1DE, data_1E0, data_1E2, data_1E4, data_1E6, data_1EA;
	static BYTE data_2AF, data_2B0;

	BYTE (&mazeScratchBuffer)[MAZE_SCRATCH_BUFFER_SIZE] = (BYTE(&)[MAZE_SCRATCH_BUFFER_SIZE])objects;

	memset(mazeScratchBuffer, 0xF, MAZE_SCRATCH_BUFFER_SIZE);
	mazeScratchBuffer[0] = 0xE;
	mazeScratchBuffer[1] = 0xD;
	data_1EA = 0x13E;

	static BYTE data_EA3[] = {8, 4, 2, 1};
	static BYTE data_EA7[] = {4, 8, 1, 2};

	for (;;)
	{
	outer_loop:
		if (!data_1EA)
			break;
		data_1DE = GetRandomRanged<MAZE_SCRATCH_BUFFER_SIZE>();
		if (mazeScratchBuffer[data_1DE] != 0xF)
			continue;
		data_1E2 = GetRandomMasked(3);
		data_1E4 = 0;
		for (;;)
		{
			if (data_1E4 > 3)
				goto outer_loop;
			data_2AF = data_1E2 & 3;
			data_1E0 = data_1DE;
			CreateMaze_helper((SHORT&)data_1E0, data_2AF);
			if (mazeScratchBuffer[data_1E0] != 0xF)
				break;
			data_1E2++;
			data_1E4++;
		}
		data_1EA--;
		mazeScratchBuffer[data_1E0] ^= data_EA7[data_2AF];
		mazeScratchBuffer[data_1DE] ^= data_EA3[data_2AF];
		data_1E2 = data_1DE;
		for (;;)
		{
			data_2AF = (BYTE)GetRandomMasked(3);
			data_2B0 = (BYTE)GetRandomMasked(3) + 1;
			data_1E0 = data_1E2;
			for (;;)
			{
				CreateMaze_helper((SHORT&)data_1E0, data_2AF);
				if (!data_2B0 || mazeScratchBuffer[data_1E0] != 0xF)
					break;
				mazeScratchBuffer[data_1E0] ^= data_EA7[data_2AF];
				mazeScratchBuffer[data_1E2] ^= data_EA3[data_2AF];
				data_1EA--;
				data_2B0--;
				data_1E2 = data_1E0;
			}
			if (data_2B0)
				goto outer_loop;
		}
	}
	for (data_1DE = 1; data_1DE <= 0x40; data_1DE++)
	{
		data_1E0 = GetRandomRanged<MAZE_SCRATCH_BUFFER_SIZE>();
		data_2AF = (BYTE)GetRandomMasked(3);
		mazeScratchBuffer[data_1E0] &= ~data_EA3[data_2AF];
		CreateMaze_helper((SHORT&)data_1E0, data_2AF);
		mazeScratchBuffer[data_1E0] &= ~data_EA7[data_2AF];
	}
	for (Uint i=0; i<_countof(maze); i++)
		maze[i] = MazeTile(0x9, ' ');
	data_1E4 = 0;
	data_1E2 = 0;
	for (data_1DE = 0; data_1DE < MAZE_HEIGHT/MAZE_CELL_HEIGHT; data_1DE++)
	{
		for (data_1E0 = 0; data_1E0 < MAZE_WIDTH/MAZE_CELL_WIDTH; data_1E0++)
		{
			if (mazeScratchBuffer[data_1E2] & 8)
				for (Uint i=0; i<MAZE_CELL_WIDTH-1; i++)
					maze[data_1E4 + 1 + i] = MazeTile(0x9, 0xCD);
			if (mazeScratchBuffer[data_1E2] & 2)
			{
				data_1E6 = data_1E4 + MAZE_WIDTH;
				for (WORD data_1E8 = 1; data_1E8 <= MAZE_CELL_HEIGHT-1; data_1E8++)
				{
					maze[data_1E6] = MazeTile(0x9, 0xBA);
					data_1E6 += MAZE_WIDTH;
				}
			}
			if (data_1DE)
				data_1E6 = data_1E2 - (!data_1E0 ? 1 : 0x11);
			else
			{
				if (!data_1E0)
					data_1E6 = MAZE_SCRATCH_BUFFER_SIZE - 1;
				else
					data_1E6 = data_1E2 + MAZE_SCRATCH_BUFFER_SIZE - 0x11;
			}
			static BYTE data_EAB[] = {0x20, 0xBA, 0xBA, 0xBA, 0xCD, 0xBC, 0xBB, 0xB9, 0xCD, 0xC8, 0xC9, 0xCC, 0xCD, 0xCA, 0xCB, 0xCE};
			maze[data_1E4].ch = data_EAB[(mazeScratchBuffer[data_1E2] & 0xA) | (mazeScratchBuffer[data_1E6] & 0x5)];
			maze[data_1E4].color = 0x9;
			data_1E4 += MAZE_CELL_WIDTH;
			data_1E2++;
		}
		data_1E4 += MAZE_WIDTH * (MAZE_CELL_HEIGHT-1);
	}
}

BYTE CreateNewObject()
{
	BYTE object = objectHead_free;
	if (object)
		objectHead_free = objects[object].next;
	return object;
}

#define EXPLOSION_SIZE(x,y) ((y)*10+(x))

#define SPRITE_SIZE(x,y) (((x)<<8)+(y))

// generator
static const WORD data_1002[] = {SPRITE_SIZE(2,2), 0xFDA, 0xEBF, 0xDC0, 0xCD9};
static const WORD data_100C[] = {SPRITE_SIZE(2,2), 0xEFF, 0xEFF, 0xEFF, 0xEFF};
static const WORD data_1016[] = {SPRITE_SIZE(2,2), 0xEDA, 0xEBF, 0xEC0, 0xED9};
static const WORD data_1020[] = {SPRITE_SIZE(2,2), 0xEFF, 0xEFF, 0xEFF, 0xEFF};
static const WORD data_102A[] = {SPRITE_SIZE(2,2), 0xDDA, 0xDBF, 0xDC0, 0xDD9};
static const WORD data_1034[] = {SPRITE_SIZE(2,2), 0xEFF, 0xEFF, 0xEFF, 0xEFF};
static const WORD data_103E[] = {SPRITE_SIZE(2,2), 0xCDA, 0xCBF, 0xCC0, 0xCD9};
static const WORD data_1048[] = {SPRITE_SIZE(2,2), 0xEFF, 0xEFF, 0xEFF, 0xEFF};
static const WORD data_1052[] = {SPRITE_SIZE(2,2), 0xBDA, 0xBBF, 0xBC0, 0xBD9};
static const WORD data_105C[] = {SPRITE_SIZE(2,2), 0xEFF, 0xEFF, 0xEFF, 0xEFF};
static const WORD data_1066[] = {SPRITE_SIZE(2,2), 0xADA, 0xABF, 0xAC0, 0xAD9};
static const WORD data_1070[] = {SPRITE_SIZE(2,2), 0xEFF, 0xEFF, 0xEFF, 0xEFF};
static const WORD data_107A[] = {SPRITE_SIZE(2,2), 0x9DA, 0x9BF, 0x9C0, 0x9D9};
static const WORD data_1084[] = {SPRITE_SIZE(2,2), 0xEFF, 0xEFF, 0xEFF, 0xEFF};
static const WORD data_108E[] = {SPRITE_SIZE(2,2), 0x4DA, 0x4BF, 0x4C0, 0x4D9};
static const WORD data_1098[] = {SPRITE_SIZE(2,2), 0xEFF, 0xEFF, 0xEFF, 0xEFF};
static const WORD *data_10A2[] = {data_1002, data_100C, data_1016, data_1020, data_102A, data_1034, data_103E, data_1048, data_1052, data_105C, data_1066, data_1070, data_107A, data_1084, data_108E, data_1098};
// player
static const WORD data_10E2[] = {SPRITE_SIZE(2,2), 0xF93, 0xF93, 0xF11, 0xF10};
static const WORD data_10EC[] = {SPRITE_SIZE(2,2), 0xF4F, 0xF4F, 0xF11, 0xF10};
static const WORD *data_10F6[] = {data_10E2, data_10EC};
// ghost
static const WORD data_10FE[] = {SPRITE_SIZE(1,1), 0x202};
// snipe
static const WORD data_1108[] = {SPRITE_SIZE(2,1), 0x201, 0x218};
static const WORD data_1112[] = {SPRITE_SIZE(2,1), 0x201, 0x21A};
static const WORD data_111C[] = {SPRITE_SIZE(2,1), 0x201, 0x219};
static const WORD data_1126[] = {SPRITE_SIZE(2,1), 0x21B, 0x201};
static const WORD *data_1130[] = {data_1108, data_1112, data_1112, data_1112, data_111C, data_1126, data_1126, data_1126};
// player bullet
static const WORD data_1150[] = {SPRITE_SIZE(1,1), 0xE09};
static const WORD data_115A[] = {SPRITE_SIZE(1,1), 0xB0F};
static const WORD *data_11D4[] = {data_1150, data_115A, data_1150, data_115A};
// snipe bullet
static const WORD data_1164[] = {SPRITE_SIZE(1,1), 0xA18};
static const WORD data_116E[] = {SPRITE_SIZE(1,1), 0xA2F};
static const WORD data_1178[] = {SPRITE_SIZE(1,1), 0xA1A};
static const WORD data_1182[] = {SPRITE_SIZE(1,1), 0xA5C};
static const WORD data_118C[] = {SPRITE_SIZE(1,1), 0xA19};
static const WORD data_1196[] = {SPRITE_SIZE(1,1), 0xA2F};
static const WORD data_11A0[] = {SPRITE_SIZE(1,1), 0xA1B};
static const WORD data_11AA[] = {SPRITE_SIZE(1,1), 0xA5C};
static const WORD *data_11B4[] = {data_1164, data_116E, data_1178, data_1182, data_118C, data_1196, data_11A0, data_11AA};
// player or generator explosion
static const WORD data_12C2[] = {SPRITE_SIZE(2,2), 0xFB0, 0xFB2, 0xFB2, 0xFB0};
static const WORD data_12CC[] = {SPRITE_SIZE(2,2), 0xBB2, 0xBB0, 0xBB0, 0xBB2};
static const WORD data_12D6[] = {SPRITE_SIZE(2,2), 0xCB0, 0xCB2, 0xCB2, 0xCB0};
static const WORD data_12E0[] = {SPRITE_SIZE(2,2), 0x4B2, 0x4B0, 0x4B0, 0x4B2};
static const WORD data_12EA[] = {SPRITE_SIZE(2,2), 0x62A, 0x60F, 0x62A, 0x60F};
static const WORD data_12F4[] = {SPRITE_SIZE(2,2), 0x807, 0x820, 0x807, 0x820};
static const WORD *data_12FE[] = {data_12C2, data_12CC, data_12D6, data_12E0, data_12EA, data_12F4};
// snipe explosion
static const WORD data_1316[] = {SPRITE_SIZE(2,1), 0xFB0, 0xFB2};
static const WORD data_1320[] = {SPRITE_SIZE(2,1), 0xBB2, 0xBB0};
static const WORD data_132A[] = {SPRITE_SIZE(2,1), 0xCB0, 0xCB2};
static const WORD data_1334[] = {SPRITE_SIZE(2,1), 0x4B2, 0x4B0};
static const WORD data_133E[] = {SPRITE_SIZE(2,1), 0x62A, 0x60F};
static const WORD data_1348[] = {SPRITE_SIZE(2,1), 0x807, 0x820};
static const WORD *data_1352[] = {data_1316, data_1320, data_132A, data_1334, data_133E, data_1348};
// ghost explosion
static const WORD data_136A[] = {SPRITE_SIZE(1,1), 0xFB2};
static const WORD data_1374[] = {SPRITE_SIZE(1,1), 0xB0F};
static const WORD data_137E[] = {SPRITE_SIZE(1,1), 0xC09};
static const WORD data_1388[] = {SPRITE_SIZE(1,1), 0x407};
static const WORD *data_1392[] = {data_136A, data_136A, data_136A, data_1374, data_137E, data_1388};

bool IsPlayer(BYTE ch)
{
	return ch == 0x93 || ch == 0x4F || ch == 0x11 || ch == 0x10;
}
bool IsGenerator(MazeTile tile)
{
#ifdef FIX_BUGS
	return tile.ch == 0xDA || tile.ch == 0xBF || tile.ch == 0xC0 || tile.ch == 0xD9 || tile.ch == 0xFF;
#else
	return wmemchr((wchar_t*)&data_1002[1], (wchar_t&)tile, _countof(data_1002)-1) || tile.ch == 0xFF;
#endif
}

#ifdef OBJECT_TABLE_BINARY_COMPATIBILITY

#define FAKE_POINTER(n) 0x##n

const WORD *FakePointerToPointer(WORD fakePtr)
{
	switch (fakePtr)
	{
	case FAKE_POINTER(1002): return data_1002;
	case FAKE_POINTER(100C): return data_100C;
	case FAKE_POINTER(1016): return data_1016;
	case FAKE_POINTER(1020): return data_1020;
	case FAKE_POINTER(102A): return data_102A;
	case FAKE_POINTER(1034): return data_1034;
	case FAKE_POINTER(103E): return data_103E;
	case FAKE_POINTER(1048): return data_1048;
	case FAKE_POINTER(1052): return data_1052;
	case FAKE_POINTER(105C): return data_105C;
	case FAKE_POINTER(1066): return data_1066;
	case FAKE_POINTER(1070): return data_1070;
	case FAKE_POINTER(107A): return data_107A;
	case FAKE_POINTER(1084): return data_1084;
	case FAKE_POINTER(108E): return data_108E;
	case FAKE_POINTER(1098): return data_1098;
	case FAKE_POINTER(10E2): return data_10E2;
	case FAKE_POINTER(10EC): return data_10EC;
	case FAKE_POINTER(10FE): return data_10FE;
	case FAKE_POINTER(1108): return data_1108;
	case FAKE_POINTER(1112): return data_1112;
	case FAKE_POINTER(111C): return data_111C;
	case FAKE_POINTER(1126): return data_1126;
	case FAKE_POINTER(1150): return data_1150;
	case FAKE_POINTER(115A): return data_115A;
	case FAKE_POINTER(1164): return data_1164;
	case FAKE_POINTER(116E): return data_116E;
	case FAKE_POINTER(1178): return data_1178;
	case FAKE_POINTER(1182): return data_1182;
	case FAKE_POINTER(118C): return data_118C;
	case FAKE_POINTER(1196): return data_1196;
	case FAKE_POINTER(11A0): return data_11A0;
	case FAKE_POINTER(11AA): return data_11AA;
	case FAKE_POINTER(12C2): return data_12C2;
	case FAKE_POINTER(12CC): return data_12CC;
	case FAKE_POINTER(12D6): return data_12D6;
	case FAKE_POINTER(12E0): return data_12E0;
	case FAKE_POINTER(12EA): return data_12EA;
	case FAKE_POINTER(12F4): return data_12F4;
	case FAKE_POINTER(1316): return data_1316;
	case FAKE_POINTER(1320): return data_1320;
	case FAKE_POINTER(132A): return data_132A;
	case FAKE_POINTER(1334): return data_1334;
	case FAKE_POINTER(133E): return data_133E;
	case FAKE_POINTER(1348): return data_1348;
	case FAKE_POINTER(136A): return data_136A;
	case FAKE_POINTER(1374): return data_1374;
	case FAKE_POINTER(137E): return data_137E;
	case FAKE_POINTER(1388): return data_1388;
	default:
		return NULL;
	}
}

WORD PointerToFakePointer(const WORD *ptr)
{
	if (ptr == data_1002) return FAKE_POINTER(1002);
	if (ptr == data_100C) return FAKE_POINTER(100C);
	if (ptr == data_1016) return FAKE_POINTER(1016);
	if (ptr == data_1020) return FAKE_POINTER(1020);
	if (ptr == data_102A) return FAKE_POINTER(102A);
	if (ptr == data_1034) return FAKE_POINTER(1034);
	if (ptr == data_103E) return FAKE_POINTER(103E);
	if (ptr == data_1048) return FAKE_POINTER(1048);
	if (ptr == data_1052) return FAKE_POINTER(1052);
	if (ptr == data_105C) return FAKE_POINTER(105C);
	if (ptr == data_1066) return FAKE_POINTER(1066);
	if (ptr == data_1070) return FAKE_POINTER(1070);
	if (ptr == data_107A) return FAKE_POINTER(107A);
	if (ptr == data_1084) return FAKE_POINTER(1084);
	if (ptr == data_108E) return FAKE_POINTER(108E);
	if (ptr == data_1098) return FAKE_POINTER(1098);
	if (ptr == data_10E2) return FAKE_POINTER(10E2);
	if (ptr == data_10EC) return FAKE_POINTER(10EC);
	if (ptr == data_10FE) return FAKE_POINTER(10FE);
	if (ptr == data_1108) return FAKE_POINTER(1108);
	if (ptr == data_1112) return FAKE_POINTER(1112);
	if (ptr == data_111C) return FAKE_POINTER(111C);
	if (ptr == data_1126) return FAKE_POINTER(1126);
	if (ptr == data_1150) return FAKE_POINTER(1150);
	if (ptr == data_115A) return FAKE_POINTER(115A);
	if (ptr == data_1164) return FAKE_POINTER(1164);
	if (ptr == data_116E) return FAKE_POINTER(116E);
	if (ptr == data_1178) return FAKE_POINTER(1178);
	if (ptr == data_1182) return FAKE_POINTER(1182);
	if (ptr == data_118C) return FAKE_POINTER(118C);
	if (ptr == data_1196) return FAKE_POINTER(1196);
	if (ptr == data_11A0) return FAKE_POINTER(11A0);
	if (ptr == data_11AA) return FAKE_POINTER(11AA);
	if (ptr == data_12C2) return FAKE_POINTER(12C2);
	if (ptr == data_12CC) return FAKE_POINTER(12CC);
	if (ptr == data_12D6) return FAKE_POINTER(12D6);
	if (ptr == data_12E0) return FAKE_POINTER(12E0);
	if (ptr == data_12EA) return FAKE_POINTER(12EA);
	if (ptr == data_12F4) return FAKE_POINTER(12F4);
	if (ptr == data_1316) return FAKE_POINTER(1316);
	if (ptr == data_1320) return FAKE_POINTER(1320);
	if (ptr == data_132A) return FAKE_POINTER(132A);
	if (ptr == data_1334) return FAKE_POINTER(1334);
	if (ptr == data_133E) return FAKE_POINTER(133E);
	if (ptr == data_1348) return FAKE_POINTER(1348);
	if (ptr == data_136A) return FAKE_POINTER(136A);
	if (ptr == data_1374) return FAKE_POINTER(1374);
	if (ptr == data_137E) return FAKE_POINTER(137E);
	if (ptr == data_1388) return FAKE_POINTER(1388);
	__debugbreak();
	return 0;
}

#else // OBJECT_TABLE_BINARY_COMPATIBILITY

#define FAKE_POINTER(n) (data_##n)
#define FakePointerToPointer(n) (n)
#define PointerToFakePointer(n) (n)

#endif // OBJECT_TABLE_BINARY_COMPATIBILITY

#ifdef USE_MODULO_LOOKUP_TABLE
static const bool data_11E8[] = {
	0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,
	0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,
	0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0
};

bool IsDiagonalDoubledPhase(BYTE n)
{
	return data_11E8[n];
}
#else
bool IsDiagonalDoubledPhase(BYTE n)
{
	return n % 3 & 1;
}
#endif

static const BYTE data_1281[] = {0xB9, 0xBA, 0xBB, 0xBC, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE};
static const BYTE data_128C[] = {1, 2, 0x18, 0x1A, 0x19, 0x1B};
static const BYTE data_1292[] = {0xB9, 0xBA, 0xBB, 0xBC, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE};
static const BYTE data_129D[] = {1, 2, 0x18, 0x1A, 0x19, 0x1B};

void FreeObject(BYTE object)
{
	objects[object].next = objectHead_free;
	objectHead_free = object;
}

bool IsObjectLocationOccupied(WORD row, BYTE column)
{
	BYTE data_C79 = ((BYTE*)currentSprite)[0];
	BYTE data_C7A = ((BYTE*)currentSprite)[1];
	WORD data_B50 = (WORD)row * MAZE_WIDTH;
	for (BYTE data_C7B = 0; data_C7B < data_C79; data_C7B++)
	{
		BYTE data_C7D = column;
		for (BYTE data_C7C = 0; data_C7C < data_C7A; data_C7C++)
		{
			if (maze[data_C7D + data_B50].ch != ' ')
				return true;
			if (++data_C7D >= MAZE_WIDTH)
				data_C7D = 0;
		}
		data_B50 += MAZE_WIDTH;
		if (data_B50 >= _countof(maze))
			data_B50 -= _countof(maze);
	}
	return false;
}

void PlotObjectToMaze() // plots object currentObject with sprite currentSprite
{
	BYTE spriteHeight = ((BYTE*)currentSprite)[0];
	BYTE spriteWidth  = ((BYTE*)currentSprite)[1];
	MazeTile *spriteTile = (MazeTile*)&currentSprite[1];
	WORD data_B52 = (WORD)currentObject->y * MAZE_WIDTH;
	for (BYTE data_C81 = 0; data_C81 < spriteHeight; data_C81++)
	{
		BYTE data_C80 = currentObject->x;
		for (BYTE data_C82 = 0; data_C82 < spriteWidth; data_C82++)
		{
			maze[data_B52 + data_C80] = *spriteTile++;
			if (++data_C80 >= MAZE_WIDTH)
				data_C80 = 0;
		}
		data_B52 += MAZE_WIDTH;
		if (data_B52 >= _countof(maze))
			data_B52 -= _countof(maze);
	}
}

void PlaceObjectInRandomUnoccupiedMazeCell()
{
	do
	{
		currentObject->x = (BYTE)GetRandomRanged<MAZE_WIDTH  / MAZE_CELL_WIDTH >() * MAZE_CELL_WIDTH  + MAZE_CELL_WIDTH /2;
		currentObject->y = (BYTE)GetRandomRanged<MAZE_HEIGHT / MAZE_CELL_HEIGHT>() * MAZE_CELL_HEIGHT + MAZE_CELL_HEIGHT/2;
	}
	while (IsObjectLocationOccupied(currentObject->y, currentObject->x));
}

void CreateGeneratorsAndPlayer()
{
	for (WORD data_B58 = 1; data_B58 <= OBJECT_LASTFREE; data_B58++)
		objects[data_B58].next = data_B58 + 1;
	objects[OBJECT_LASTFREE].next = 0;
	objectHead_free = 1;
	objectHead_bullets = 0;
	objectHead_explosions = 0;
	objectHead_ghosts = 0;
	objectHead_snipes = 0;
	objectHead_generators = 0;
	for (WORD data_B58 = 0; data_B58 < numGeneratorsAtStart; data_B58++)
	{
		BYTE newGenerator = CreateNewObject();
		objects[newGenerator].next = objectHead_generators;
		objectHead_generators = newGenerator;
		currentObject = &objects[newGenerator];
		Generator &generator = *(Generator*)currentObject;
		generator.sprite = FAKE_POINTER(1002);
		currentSprite = data_1002;
		PlaceObjectInRandomUnoccupiedMazeCell();
		generator.unused = 0;
		generator.animFrame = (BYTE)GetRandomMasked(0xF);
		generator.spawnFrame = 1;
		PlotObjectToMaze();
	}
	numGenerators = numGeneratorsAtStart;
	numGhostsKilled = 0;
	numGhosts = 0;
	numBullets = 0;
	numSnipesKilled = 0;
	numPlayerDeaths = 0;
	numSnipes = 0;
	score = 0;
	playerAnimFrame = false;
	data_C74 = 0;
	isPlayerDying = false;
	isPlayerExploding = false;
	player.sprite = FAKE_POINTER(10E2);
	currentSprite = data_10E2;
	currentObject = &player;
	PlaceObjectInRandomUnoccupiedMazeCell();
	PlotObjectToMaze();
	viewportFocusX = player.x;
	viewportFocusY = player.y;
	player.inputFrame = 1;
	player.firingFrame = 1;
}

void SetSoundEffectState(BYTE frame, SoundEffect index)
{
	if (currentSoundEffect != SoundEffect_None && index < currentSoundEffect)
		return;
	if (!shooting_sound_enabled && !index)
		return;
	currentSoundEffectFrame = frame;
	currentSoundEffect      = index;
}

bool UpdateHUD() // returns true if the match has been won
{
	frame++;
	if (lastHUD_numSnipesKilled != numSnipesKilled)
		outputNumber(0x13, 0, 4, 0, 11, lastHUD_numSnipesKilled = numSnipesKilled);
	if (lastHUD_numGhostsKilled != numGhostsKilled)
		outputNumber(0x13, 0, 4, 0, 20, lastHUD_numGhostsKilled = numGhostsKilled);
	if (lastHUD_numGeneratorsKilled != numGenerators)
	{
		outputNumber(0x17, 0, 2, 1,  3, lastHUD_numGeneratorsKilled = numGenerators);
		outputNumber(0x13, 0, 2, 0,  3, numGeneratorsAtStart - numGenerators);
	}
	if (lastHUD_numSnipes != numSnipes)
		outputNumber(0x17, 0, 3, 1, 12, lastHUD_numSnipes = numSnipes);
	if (lastHUD_numGhosts != numGhosts)
		outputNumber(0x17, 0, 3, 1, 21, lastHUD_numGhosts = numGhosts);
	if (lastHUD_score != score)
	{
		lastHUD_score = score;
		if (lastHUD_score > 0)
			outputNumber(0x17, 1, 5, 2, 33, lastHUD_score);
		else
			outputText  (0x17,    6, 2, 33, statusLine+65);
	}
	if (lastHUD_numPlayerDeaths != numPlayerDeaths)
	{
		BYTE livesRemaining = numLives - (lastHUD_numPlayerDeaths = numPlayerDeaths);
		if (livesRemaining == 1)
			outputText  (0x1C,   10, 2,  0, statusLine+71);
		else
		{
			{}	outputNumber(0x17, 0, 1, 2,  0, livesRemaining);
			if (!livesRemaining)
			{
				outputNumber(0x1C, 0, 1, 2,  0, 0);
				outputText  (0x1C,    1, 2,  3, statusLine+81);
			}
		}
	}
	outputNumber(0x17, 0, 5, 1, 34, frame); // Time

	if (numSnipes || numGenerators || numGhosts)
		return false;

	EraseBottomTwoLines();
	CloseDirectConsole(WINDOW_HEIGHT-2);
	WriteTextToConsole("Congratulations --- YOU ACTUALLY WON!!!\r\n");
	return true;
}

void ExplodeObject(BYTE arg)
{
	if (arg == OBJECT_PLAYER) // explode the player (and don't overwrite the player object)
	{
		arg = OBJECT_PLAYEREXPLOSION;
		playerExplosion.x = player.x;
		playerExplosion.y = player.y;
		playerExplosion.spriteSize = EXPLOSION_SIZE(2,2);
		playerExplosion.sprite = FAKE_POINTER(12C2);
		isPlayerExploding = true;
	}
	currentObject = &objects[arg];
	currentObject->next = objectHead_explosions;
	objectHead_explosions = arg;
	const WORD *data_CC4 = FakePointerToPointer(currentObject->sprite);
	BYTE data_CC8 = ((BYTE*)data_CC4)[0];
	BYTE data_CC9 = ((BYTE*)data_CC4)[1];
	Explosion &explosion = *(Explosion*)currentObject;
	if (data_CC8 == 2 && data_CC9 == 2)
	{
		explosion.sprite = FAKE_POINTER(12C2);
		currentSprite = data_12C2;
		explosion.spriteSize = EXPLOSION_SIZE(2,2);
		explosion.animFrame = 0;
		SetSoundEffectState(0, SoundEffect_ExplodePlayer);
	}
	if (data_CC8 == 1 && data_CC9 == 2)
	{
		explosion.sprite = FAKE_POINTER(1316);
		currentSprite = data_1316;
		explosion.spriteSize = EXPLOSION_SIZE(2,1);
		explosion.animFrame = 0;
		SetSoundEffectState(0, SoundEffect_ExplodeSnipe);
	}
	if (data_CC8 == 1 && data_CC9 == 1)
	{
		explosion.sprite = FAKE_POINTER(136A);
		currentSprite = data_136A;
		explosion.spriteSize = EXPLOSION_SIZE(1,1);
		explosion.animFrame = 2;
		SetSoundEffectState(2, SoundEffect_ExplodeGhost);
	}
	PlotObjectToMaze();
}

void FreeObjectInList_worker(BYTE *objectHead, BYTE object)
{
	BYTE data_CCA = *objectHead;
	if (object == data_CCA)
	{
		*objectHead = objects[object].next;
		return;
	}
	for (;;)
	{
		BYTE data_CCB = objects[data_CCA].next;
		if (!data_CCB)
			return;
		if (object == data_CCB)
			break;
		data_CCA = data_CCB;
	}
	objects[data_CCA].next = objects[object].next;
}

void FreeObjectInList(BYTE *objectHead, BYTE object)
{
	if (object == OBJECT_PLAYER)
	{
		ExplodeObject(object);
		return;
	}
	FreeObjectInList_worker(objectHead, object);
	if (objectHead != &objectHead_bullets)
	{
		ExplodeObject(object);
		return;
	}
	FreeObject(object);
}

bool MoveBulletAndTestHit(OrthogonalDirection arg)
{
	switch (bulletCollisionDirection = arg)
	{
	case OrthogonalDirection_Up:
		bulletTestPos -= MAZE_WIDTH;
		if (--currentObject->y == 0xFF)
		{
			currentObject->y = MAZE_HEIGHT - 1;
			bulletTestPos += _countof(maze);
		}
		break;
	case OrthogonalDirection_Right:
		bulletTestPos++;
		if (++currentObject->x >= MAZE_WIDTH)
		{
			currentObject->x = 0;
			bulletTestPos -= MAZE_WIDTH;
		}
		break;
	case OrthogonalDirection_Down:
		bulletTestPos += MAZE_WIDTH;
		if (++currentObject->y >= MAZE_HEIGHT)
		{
			currentObject->y = 0;
			bulletTestPos -= _countof(maze);
		}
		break;
	case OrthogonalDirection_Left:
		bulletTestPos--;
		if (--currentObject->x == 0xFF)
		{
			currentObject->x = MAZE_WIDTH - 1;
			bulletTestPos += MAZE_WIDTH;
		}
		break;
	default:
		__assume(0);
	}
	return bulletTestPos->ch != ' ';
}

void UpdateBullets()
{
	BYTE prevObject = 0;
	for (BYTE object = objectHead_bullets; object;)
	{
		currentObject = &objects[object];
		Bullet &bullet = *(Bullet*)currentObject;
		bulletTestPos = &maze[bullet.y * MAZE_WIDTH + bullet.x];
		if (bulletTestPos->ch == 0xB2)
		{
			BYTE nextObject = bullet.next;
			*bulletTestPos = MazeTile(0x9, ' ');
			FreeObjectInList(&objectHead_bullets, object);
			numBullets--;
			object = nextObject;
			continue;
		}
		*bulletTestPos = MazeTile(0x9, ' ');
		switch (bullet.moveDirection)
		{
		case MoveDirection_UpRight:
			if (MoveBulletAndTestHit(OrthogonalDirection_Right) || IsDiagonalDoubledPhase(bullet.y) && MoveBulletAndTestHit(OrthogonalDirection_Right))
				break;
			goto case_MoveDirection_Up;
		case MoveDirection_DownRight:
			if (MoveBulletAndTestHit(OrthogonalDirection_Down) || IsDiagonalDoubledPhase(bullet.y) && MoveBulletAndTestHit(OrthogonalDirection_Right))
				break;
			// fall through
		case MoveDirection_Right:
			if (MoveBulletAndTestHit(OrthogonalDirection_Right))
				break;
			goto main_139A;
		case MoveDirection_Down:
			if (MoveBulletAndTestHit(OrthogonalDirection_Down))
				break;
			goto main_139A;
		case MoveDirection_DownLeft:
			if (MoveBulletAndTestHit(OrthogonalDirection_Down) || IsDiagonalDoubledPhase(bullet.y) && MoveBulletAndTestHit(OrthogonalDirection_Left))
				break;
			// fall through
		case MoveDirection_Left:
			if (MoveBulletAndTestHit(OrthogonalDirection_Left))
				break;
			goto main_139A;
		case MoveDirection_UpLeft:
			if (MoveBulletAndTestHit(OrthogonalDirection_Left) || IsDiagonalDoubledPhase(bullet.y) && MoveBulletAndTestHit(OrthogonalDirection_Left))
				break;
			// fall through
		case MoveDirection_Up:
		case_MoveDirection_Up:
			if (MoveBulletAndTestHit(OrthogonalDirection_Up))
				break;
		main_139A:
			if (bullet.bulletType!=BulletType_Player)
				currentSprite = FakePointerToPointer(bullet.sprite);
			else
			{
				if (++bullet.animFrame > 3)
					bullet.animFrame = 0;
				currentSprite = data_11D4[bullet.animFrame];
			}
			*bulletTestPos = (MazeTile&)currentSprite[1];
			prevObject = object;
			object = bullet.next;
			continue;
		default:
			__assume(0);
		}
		BYTE find_this = bulletTestPos->ch;
		if (!memchr(data_1281, find_this, _countof(data_1281)))
		{
			if (bullet.bulletType==BulletType_Player)
			{
				if (memchr(data_128C, find_this, _countof(data_128C)))
					score += 1;
				else
				if (IsGenerator(*bulletTestPos))
					score += 50;
			}
			else
			if (generatorsResistSnipeBullets && IsGenerator(*bulletTestPos))
				goto main_150E;
			*bulletTestPos = MazeTile(0xF, 0xB2);
		}
		else
		if (enableRubberBullets && bullet.bulletType==BulletType_Player && bulletLifetime[object] && (bullet.moveDirection & MoveDirectionMask_Diagonal))
		{
			bulletLifetime[object]--;
			bullet.moveDirection = (MoveDirection)bulletBounceTable[bulletCollisionDirection][bullet.moveDirection];
			SetSoundEffectState(1, SoundEffect_PlayerBullet);
			MoveBulletAndTestHit((bulletCollisionDirection + 2) & OrthogonalDirectionMask_All);
			goto main_139A;
		}
	main_150E:
		numBullets--;
		if (!prevObject)
			objectHead_bullets = bullet.next;
		else
			objects[prevObject].next = bullet.next;
		BYTE nextObject = bullet.next;
		FreeObject(object);
		object = nextObject;
	}
}

struct OrthoDistanceInfo
{
	union
	{
		struct {BYTE x, y;};
		WORD xy;
	};
	MoveDirection direction;
};

OrthoDistanceInfo GetOrthoDistanceAndDirection(Object &object)
// Calculates the orthogonal distance between object and the viewport focus
{
	OrthoDistanceInfo result;
	BYTE bx = 1;
	int  ax = object.x - viewportFocusX;
	if (ax <= 0)
	{
		bx = 0;
		ax = -ax;
	}
	if (ax >= MAZE_WIDTH/2)
	{
		bx ^= 1;
		ax = MAZE_WIDTH - ax;
	}
	result.x = ax;
	ax = object.y - viewportFocusY;
	if (ax < 0)
	{
		bx += 2;
		ax = -ax;
	}
	if (ax >= MAZE_HEIGHT/2)
	{
		bx ^= 2;
		ax = MAZE_HEIGHT - ax;
	}
	result.y = ax;
	orthoDistance = result.x + result.y;
	if (result.x == 0)
	{
		result.direction = (MoveDirection)(bx * 2);
		return result;
	}
	if (result.y == 0)
	{
		result.direction = (MoveDirection)(bx * 4 + 2);
		return result;
	}
	static const MoveDirection diagonalDirectionTable[] = {MoveDirection_UpRight, MoveDirection_UpLeft, MoveDirection_DownRight, MoveDirection_DownLeft};
	result.direction = diagonalDirectionTable[bx];
	return result;
}

struct MoveObject_retval {bool al; BYTE ah; WORD cx; MazeTile *bx_si;};
MoveObject_retval MoveObject(MovingObject &object)
{
	union
	{
		WORD ax;
		struct {BYTE al, ah;};
	};
	union
	{
		WORD cx;
		struct {BYTE cl, ch;};
	};
	union
	{
		WORD dx;
		struct {BYTE dl, dh;};
	};
	ax = cx = object.xy;
	dx = 0;
	int tmp;
	switch (object.moveDirection)
	{
	case MoveDirection_UpRight:
		tmp = cl + (dl = IsDiagonalDoubledPhase(ch) + 1);
		if (tmp >= MAZE_WIDTH)
			tmp -= MAZE_WIDTH;
		cl = tmp;
		// fall through
	case MoveDirection_Up:
		dh++;
		tmp = ch - 1;
		if (tmp < 0)
			tmp = MAZE_HEIGHT - 1;
		ch = tmp;
		ah = ch;
		break;
	case MoveDirection_Right:
		dl++;
		tmp = cl + 1;
		if (tmp >= MAZE_WIDTH)
			tmp = 0;
		cl = tmp;
		break;
	case MoveDirection_DownRight:
		dh++;
		tmp = ch + 1;
		if (tmp >= MAZE_HEIGHT)
			tmp = 0;
		ch = tmp;
		dl = IsDiagonalDoubledPhase(ch) + 1;
		tmp = cl + dl;
		if (tmp >= MAZE_WIDTH)
			tmp -= MAZE_WIDTH;
		cl = tmp;
		break;
	case MoveDirection_Down:
		dh++;
		tmp = ch + 1;
		if (tmp >= MAZE_HEIGHT)
			tmp = 0;
		ch = tmp;
		break;
	case MoveDirection_DownLeft:
		dh++;
		tmp = ch + 1;
		if (tmp >= MAZE_HEIGHT)
			tmp = 0;
		ch = tmp;
		dl = IsDiagonalDoubledPhase(ch) + 1;
		tmp = cl - dl;
		if (tmp < 0)
			tmp += MAZE_WIDTH;
		cl = tmp;
		al = cl;
		break;
	case MoveDirection_Left:
		dl++;
		tmp = cl - 1;
		if (tmp < 0)
			tmp = MAZE_WIDTH - 1;
		cl = tmp;
		al = cl;
		break;
	case MoveDirection_UpLeft:
		dl = IsDiagonalDoubledPhase(ch) + 1;
		tmp = cl - dl;
		if (tmp < 0)
			tmp += MAZE_WIDTH;
		cl = tmp;
		dh++;
		tmp = ch - 1;
		if (tmp < 0)
			tmp = MAZE_HEIGHT - 1;
		ch = tmp;
		ax = cx;
		break;
	default:
		__assume(0);
	}
	const WORD *ptr = FakePointerToPointer(object.sprite);
	dl += ((BYTE*)ptr)[1];
	dh += ((BYTE*)ptr)[0];
	for (MazeTile *si = &maze[ah * MAZE_WIDTH];;)
	{
		ah = dl;
		for (size_t bx = al;;)
		{
			if (si[bx].ch != ' ')
			{
				MoveObject_retval retval;
				retval.al = true;
				retval.ah = si[bx].ch;
				retval.cx = cx;
				retval.bx_si = &si[bx];
				return retval;
			}
			if (--ah == 0)
				break;
			if (++bx >= MAZE_WIDTH)
				bx = 0;
		}
		if (--dh == 0)
			break;
		si += MAZE_WIDTH;
		if (si >= &maze[_countof(maze)])
			si -=       _countof(maze);
	}
	data_C77 = cl;
	data_C78 = ch;
	MoveObject_retval retval;
	retval.al = false;
	retval.ah = 0;
	retval.cx = cx;
	return retval;
}

void FireBullet(BYTE bulletType)
{
	MovingObject &shooter = *(Bullet*)currentObject;
	MoveDirection fireDirection = shooter.moveDirection;
	BYTE data_C98 = shooter.x;
	BYTE data_C99 = shooter.y;
	switch (fireDirection)
	{
	case MoveDirection_UpRight:
		data_C98 += ((BYTE*)currentSprite)[1];
		goto case_MoveDirection_Up;
	case MoveDirection_Right:
		data_C98 += ((BYTE*)currentSprite)[1];
		break;
	case MoveDirection_DownRight:
		data_C98 += ((BYTE*)currentSprite)[1];
		goto main_168F;
	case MoveDirection_Down:
		data_C99 += ((BYTE*)currentSprite)[0];
		data_C98 = data_C98 + ((BYTE*)currentSprite)[1] - 1;
		break;
	case MoveDirection_DownLeft:
		data_C98--;
	main_168F:
		data_C99 += ((BYTE*)currentSprite)[0];
		break;
	case MoveDirection_Left:
		data_C98--;
		break;
	case MoveDirection_UpLeft:
		data_C98--;
		// fall through
	case MoveDirection_Up:
	case_MoveDirection_Up:
		data_C99--;
		break;
	default:
		__assume(0);
	}
	if (data_C98 >= MAZE_WIDTH)
	{
		if (data_C98 > 240)
			data_C98 += MAZE_WIDTH;
		else
			data_C98 -= MAZE_WIDTH;
	}
	if (data_C99 >= MAZE_HEIGHT)
	{
		if (data_C99 > 240)
			data_C99 += MAZE_HEIGHT;
		else
			data_C99 -= MAZE_HEIGHT;
	}
	const WORD *currentSprite_backup = currentSprite;
	currentSprite = data_1150;
	if (IsObjectLocationOccupied(data_C99, data_C98))
	{
		WORD data_B5E = data_C99 * MAZE_WIDTH + data_C98;
		if (memchr(data_1292, maze[data_B5E].ch, _countof(data_1292)))
			goto main_1899;
		if (bulletType==BulletType_Player)
		{
			if (memchr(data_129D, maze[data_B5E].ch, _countof(data_129D)))
				score += 1;
			else
			if (IsGenerator(maze[data_B5E]))
				score += 50;
		}
		else
		if (generatorsResistSnipeBullets && IsGenerator(maze[data_B5E]))
			goto main_1899;
		maze[data_B5E] = MazeTile(0xF, 0xB2);
		goto main_1899;
	}
	BYTE newBullet = CreateNewObject();
	if (!newBullet || numBullets > 50)
		goto main_1899;
	numBullets++;
	Object *currentObject_backup = currentObject;
	currentObject = &objects[newBullet];
	Bullet &bullet = *(Bullet*)currentObject;
	if (!objectHead_bullets)
		objectHead_bullets = newBullet;
	else
	{
		BYTE objectTail_bullets = objectHead_bullets;
		while (BYTE nextObject = objects[objectTail_bullets].next)
			objectTail_bullets = nextObject;
		objects[objectTail_bullets].next = newBullet;
	}
	bullet.next = 0;
	if (bulletType==BulletType_Player)
		bullet.sprite = FAKE_POINTER(1150);
	else
		bullet.sprite = PointerToFakePointer(data_11B4[fireDirection]);
	bullet.x = data_C98;
	bullet.y = data_C99;
	bullet.moveDirection = fireDirection;
	bullet.animFrame = 0;
	bullet.bulletType = bulletType;
	bulletLifetime[newBullet] = (BYTE)GetRandomMasked(7) + 1;
	currentSprite = FakePointerToPointer(bullet.sprite);
	PlotObjectToMaze();
	currentObject = currentObject_backup;
main_1899:
	currentSprite = currentSprite_backup;
}

bool FireSnipeBullet()
{
	BYTE data_C9C = orthoDistance >> snipeShootingAccuracy;
	if (data_C9C > 10)
		return false;
	if (GetRandomMasked(0xFFFF >> (15 - data_C9C)))
		return false;
	FireBullet(BulletType_Snipe);
	SetSoundEffectState(0, SoundEffect_SnipeBullet);
	return true;
}

void UpdateSnipes()
{
	for (BYTE object = objectHead_snipes; object;)
	{
		Snipe &snipe = (Snipe&)objects[object];
		MazeTile *snipeMazeRow = &maze[snipe.y * MAZE_WIDTH];
		MazeTile * leftPart = &snipeMazeRow[snipe.x];
		MazeTile *rightPart = snipe.x >= MAZE_WIDTH-1 ? &snipeMazeRow[0] : leftPart + 1;
		MazeTile *ghostPart;
		if (leftPart->ch == 0xB2)
		{
			*leftPart = MazeTile(0x9, ' ');
			ghostPart = rightPart;
		}
		else
		if (rightPart->ch == 0xB2)
		{
			*rightPart = MazeTile(0x9, ' ');
			ghostPart = leftPart;
		}
		else
		if (--snipe.moveFrame == 0)
		{
			* leftPart = MazeTile(0x9, ' ');
			*rightPart = MazeTile(0x9, ' ');
			if (GetRandomMasked(3) != 0) // 3/4 chance of moving in the same direction as before
			{
				MoveObject_retval result = MoveObject(snipe);
				if (!result.al)
				{
					snipe.xy = result.cx;
					if (!(snipe.moveDirection & MoveDirectionMask_Diagonal))
						goto main_2021;
					snipe.moveFrame = 8;
					goto main_2025;
				}
			}
			if (GetRandomMasked(3) == 0) // 1/4 * 1/4 chance of moving in a random direction
				snipe.moveDirection = (MoveDirection)GetRandomMasked(MoveDirectionMask_All);
			else                         // 1/4 * 3/4 chance of moving toward the player
				snipe.moveDirection = GetOrthoDistanceAndDirection(snipe).direction;
			for (Uint count=8; count; count--)
			{
				MoveObject_retval result = MoveObject(snipe);
				if (!result.al)
					break;
				if (snipe.movementFlags & EnemyMovementFlag_TurnDirection)
					snipe.moveDirection = (snipe.moveDirection - 1) & MoveDirectionMask_All;
				else
					snipe.moveDirection = (snipe.moveDirection + 1) & MoveDirectionMask_All;
			}
			snipe.sprite = PointerToFakePointer(data_1130[snipe.moveDirection]);
		main_2021:
			snipe.moveFrame = 6;
		main_2025:
			const WORD *sprite = FakePointerToPointer(snipe.sprite);
			maze[snipe.y * MAZE_WIDTH + snipe.x] = (MazeTile&)sprite[1 + 0];
			if (snipe.x < MAZE_WIDTH-1)
				maze[snipe.y * MAZE_WIDTH + snipe.x+1] = (MazeTile&)sprite[1 + 1];
			else
				maze[snipe.y * MAZE_WIDTH            ] = (MazeTile&)sprite[1 + 1];
			OrthoDistanceInfo orthoDist = GetOrthoDistanceAndDirection(snipe);
			MoveDirection al = orthoDist.direction;
			MoveDirection ah = snipe.moveDirection;
			if (al != ah)
				goto main_209E;
			if (orthoDist.x && orthoDist.y)
			{
				if (abs((int)orthoDist.x * MAZE_CELL_HEIGHT - (int)orthoDist.y * MAZE_CELL_WIDTH) >= MAZE_CELL_WIDTH)
					goto next_snipe;
				al = snipe.moveDirection;
			}
			for (;;)
			{
				{
					BYTE moveDirection = snipe.moveDirection;
					snipe.moveDirection = al;
					currentObject = &snipe;
					currentSprite = FakePointerToPointer(snipe.sprite);
					bool tmp = FireSnipeBullet();
					snipe.moveDirection = moveDirection;
					if (!tmp)
						break;
				}
				if (maze[snipe.y * MAZE_WIDTH + snipe.x].ch != 0x01)
					maze[snipe.y * MAZE_WIDTH + snipe.x] = MazeTile(0x9, 0xFF);
				else
				{
					if (snipe.x < MAZE_WIDTH-1)
						maze[snipe.y * MAZE_WIDTH + snipe.x+1] = MazeTile(0x9, 0xFF);
					else
						maze[snipe.y * MAZE_WIDTH            ] = MazeTile(0x9, 0xFF);
				}
				break;
			main_209E:
				if (orthoDist.y <= 2 && al != MoveDirection_Up && al != MoveDirection_Down)
				{
					if (al <= MoveDirection_Down)
					{
						if (inrange(ah, MoveDirection_UpRight, MoveDirection_DownRight))
						{
							al = MoveDirection_Right;
							continue;
						}
					}
					else
					if (ah >= MoveDirection_DownLeft)
					{
						al = MoveDirection_Left;
						continue;
					}
				}
				if (orthoDist.x > 2)
					break;
				al = (al + 1) & MoveDirectionMask_All;
				if (al == MoveDirection_UpLeft || al == MoveDirection_DownRight)
					break;
				if (al < MoveDirection_Down)
				{
					ah = (ah + 1) & MoveDirectionMask_All;
					if (ah > MoveDirection_Right)
						break;
					al = MoveDirection_Up;
					continue;
				}
				ah = (ah + 2) & MoveDirectionMask_All;
				if (ah <= MoveDirection_Down)
					break;
				al = MoveDirection_Down;
			}
			goto next_snipe;
		}
		else
		{
		next_snipe:
			object = snipe.next;
			continue;
		}
		if (enableGhostSnipes && ghostPart->ch == 0x01)
		{
			*ghostPart = MazeTile(0x5, 0x02);
			Ghost &ghost = (Ghost&)snipe;
			ghost.x = ghostPart - snipeMazeRow;

			// TODO: make the typecasting here less hacky; currently it only works because "next" is the first member of struct Object
			for (Object *objectInList = (Object*)&objectHead_snipes;;)
			{
				Object *prevObject = objectInList;
				objectInList = &objects[objectInList->next];
				if (objectInList == &ghost)
				{
					BYTE nextObject = prevObject->next = ghost.next;
					ghost.next = objectHead_ghosts;
					objectHead_ghosts = object;
					object = nextObject;
					break;
				}
			}
			ghost.moveFrame = 2;
			ghost.sprite = FAKE_POINTER(10FE);
			numGhosts++;
		}
		else
		{
			BYTE nextObject = snipe.next;
			FreeObjectInList(&objectHead_snipes, object);
			object = nextObject;
		}
		numSnipes--;
		numSnipesKilled++;
	}
}

void UpdateGhosts()
{
	for (BYTE object = objectHead_ghosts; object;)
	{
		Ghost &ghost = (Ghost&)objects[object];
		MazeTile &ghostInMaze = maze[ghost.y * MAZE_WIDTH + ghost.x];
		if (ghostInMaze.ch != 0xB2)
		{
			if (--ghost.moveFrame)
			{
				object = ghost.next;
				continue;
			}
		}
		else
		{
	kill_ghost:
			BYTE nextObject = ghost.next;
			FreeObjectInList(&objectHead_ghosts, object);
			object = nextObject;
			numGhosts--;
			numGhostsKilled++;
			continue;
		}
		ghostInMaze = MazeTile(0x9, ' ');
		if (!(ghost.movementFlags & EnemyMovementFlag_GhostMoveStraight))
		{
			OrthoDistanceInfo orthoDist = GetOrthoDistanceAndDirection(ghost);
			if (orthoDistance <= 4)
			{
				ghost.moveDirection = orthoDist.direction;
				MoveObject_retval result = MoveObject(ghost);
				if (result.al && (ghost.moveDirection & MoveDirectionMask_Diagonal))
					if (IsPlayer(result.ah))
					{
						if (GetRandomMasked(ghostBitingAccuracy) == 0)
						{
							result.bx_si->ch = 0xB2;
							goto kill_ghost;
						}
#ifndef FIX_BUGS
						// a compiler bug manifesting in the original game caused the CX register to be overwritten by the call to GetRandomMasked()
						orthoDist.xy = 947;
#endif
					}
			}
			if (orthoDist.y == 1)
				orthoDist.direction = orthoDist.direction >= MoveDirection_Down ? MoveDirection_Left : MoveDirection_Right;
			else
			if (orthoDist.y < 1)
			{
				ghost.moveDirection = ++orthoDist.direction;
				MoveObject_retval result = MoveObject(ghost);
				if (!result.al)
				{
					ghost.xy = result.cx;
					goto plot_ghost_and_continue;
				}
				orthoDist.direction -= 2;
			}
			else
			if (orthoDist.x == 1)
				orthoDist.direction = (orthoDist.direction + 1) & MoveDirectionMask_UpDown;
			else
			if (orthoDist.x < 1)
			{
				ghost.moveDirection = orthoDist.direction += 2;
				MoveObject_retval result = MoveObject(ghost);
				if (!result.al)
				{
					ghost.xy = result.cx;
					goto plot_ghost_and_continue;
				}
				orthoDist.direction = (orthoDist.direction - 4) & MoveDirectionMask_All;
			}
			ghost.moveDirection = orthoDist.direction;
			{
				MoveObject_retval result = MoveObject(ghost);
				if (!result.al)
				{
					ghost.xy = result.cx;
					goto plot_ghost_and_continue;
				}
			}
			if (orthoDistance >= 20)
				ghost.movementFlags |= EnemyMovementFlag_GhostMoveStraight;
			ghost.moveDirection = (MoveDirection)GetRandomMasked(MoveDirectionMask_All);
		}
		for (Uint count=8; count; count--)
		{
			MoveObject_retval result = MoveObject(ghost);
			if (!result.al)
			{
				ghost.xy = result.cx;
				break;
			}
			ghost.movementFlags &= ~EnemyMovementFlag_GhostMoveStraight;
			if (ghost.movementFlags & EnemyMovementFlag_TurnDirection)
				ghost.moveDirection = (ghost.moveDirection - 1) & MoveDirectionMask_All;
			else
				ghost.moveDirection = (ghost.moveDirection + 1) & MoveDirectionMask_All;
		}
	plot_ghost_and_continue:
		maze[ghost.y * MAZE_WIDTH + ghost.x] = MazeTile(0x5, 0x02);
		ghost.moveFrame = 3;
		object = ghost.next;
	}
}

bool IsObjectTaggedToExplode()
{
	BYTE spriteHeight = ((BYTE*)currentSprite)[0];
	BYTE spriteWidth  = ((BYTE*)currentSprite)[1];
	WORD data_B56 = currentObject->y * MAZE_WIDTH;
	for (BYTE row = 0; row < spriteHeight; row++)
	{
		BYTE data_C8B = currentObject->x;
		for (BYTE column = 0; column < spriteWidth; column++)
		{
			if (maze[data_B56 + data_C8B].ch == 0xB2)
				return true;
			data_C8B++;
			if (data_C8B >= MAZE_WIDTH)
				data_C8B = 0;
		}
		data_B56 += MAZE_WIDTH;
		if (data_B56 >= _countof(maze))
			data_B56 -= _countof(maze);
	}
	return false;
}

void EraseObjectFromMaze()
{
	BYTE spriteHeight = ((BYTE*)currentSprite)[0];
	BYTE spriteWidth  = ((BYTE*)currentSprite)[1];
	WORD data_B54 = currentObject->y * MAZE_WIDTH;
	for (BYTE row = 0; row < spriteHeight; row++)
	{
		BYTE data_C86 = currentObject->x;
		for (BYTE column = 0; column < spriteWidth; column++)
		{
			maze[data_B54 + data_C86] = MazeTile(0x9, ' ');
			if (++data_C86 >= MAZE_WIDTH)
				data_C86 = 0;
		}
		data_B54 += MAZE_WIDTH;
		if (data_B54 >= _countof(maze))
			data_B54 -= _countof(maze);
	}
}

void UpdateGenerators()
{
	for (BYTE object = objectHead_generators; object;)
	{
		currentObject = &objects[object];
		Generator &generator = *(Generator*)currentObject;
		if (++generator.animFrame > 15)
			generator.animFrame = 0;
		currentSprite = data_10A2[generator.animFrame];
		generator.sprite = PointerToFakePointer(currentSprite);
		if (IsObjectTaggedToExplode())
		{
			BYTE nextObject = generator.next;
			FreeObjectInList(&objectHead_generators, object);
			object = nextObject;
			numGenerators--;
			continue;
		}
		EraseObjectFromMaze();
		PlotObjectToMaze();
		if (--generator.spawnFrame)
			goto next_generator;
		GetOrthoDistanceAndDirection(*currentObject);
		if (frame >= 0xF00)
			generator.spawnFrame = 5;
		else
			generator.spawnFrame = 5 + (orthoDistance >> (frame/0x100 + 1));
		if (GetRandomMasked(0xF >> (numGeneratorsAtStart - numGenerators)))
			goto next_generator;
		currentSprite = data_1112;
		BYTE x = generator.x + 2;
#ifdef EMULATE_LATENT_BUGS
		if (x  > MAZE_WIDTH - 1)
			x -= MAZE_WIDTH - 1;
#else
		if (x >= MAZE_WIDTH)
			x -= MAZE_WIDTH;
#endif
		BYTE y = generator.y;
		if (IsObjectLocationOccupied(y, x))
			goto next_generator;
		if (numSnipes + numGhosts < maxSnipes)
		{
			BYTE spawnedSnipeIndex = CreateNewObject();
			if (!spawnedSnipeIndex)
				goto next_generator;
			numSnipes++;
			object = generator.next;
			currentObject = &objects[spawnedSnipeIndex];
			Snipe &spawnedSnipe = *(Snipe*)currentObject;
			spawnedSnipe.next = objectHead_snipes;
			objectHead_snipes = spawnedSnipeIndex;
			spawnedSnipe.x = x;
			spawnedSnipe.y = y;
			spawnedSnipe.moveDirection = MoveDirection_Right;
			spawnedSnipe.sprite = FAKE_POINTER(1112);
			PlotObjectToMaze();
			spawnedSnipe.movementFlags = (BYTE)GetRandomMasked(1); // randomly set or clear EnemyMovementFlag_TurnDirection
			spawnedSnipe.moveFrame = 4;
			continue;
		}
	next_generator:
		object = generator.next;
	}
}

bool MovePlayer_helper(OrthogonalDirection arg)
{
	player.moveDirection = OrthoDirectionToMoveDirection(arg);
	MoveObject_retval result = MoveObject(*(MovingObject*)currentObject);
	if (result.al)
		return data_CBF = true;
	viewportFocusX = player.x = data_C77;
	viewportFocusY = player.y = data_C78;
	return false;
}

bool MovePlayer()
{
	MoveDirection moveDirection = player.moveDirection;
	data_CBF = false;
	switch (moveDirection)
	{
	case MoveDirection_UpRight:
		MovePlayer_helper(OrthogonalDirection_Right);
		if (!IsDiagonalDoubledPhase(currentObject->y))
			goto case_MoveDirection_Up;
		if (MovePlayer_helper(OrthogonalDirection_Right))
			goto case_MoveDirection_Up;
		if (!MovePlayer_helper(OrthogonalDirection_Up))
			goto main_1A64;
		goto case_MoveDirection_Left;
	case MoveDirection_DownRight:
		if (MovePlayer_helper(OrthogonalDirection_Down))
			goto case_MoveDirection_Right;
		if (IsDiagonalDoubledPhase(currentObject->y))
			MovePlayer_helper(OrthogonalDirection_Right);
		goto case_MoveDirection_Right;
	case MoveDirection_Down:
		MovePlayer_helper(OrthogonalDirection_Down);
		goto main_1A64;
	case MoveDirection_DownLeft:
		if (MovePlayer_helper(OrthogonalDirection_Down))
			goto case_MoveDirection_Left;
		if (!IsDiagonalDoubledPhase(currentObject->y))
			goto case_MoveDirection_Left;
		MovePlayer_helper(OrthogonalDirection_Left);
		// fall through
	case MoveDirection_Left:
	case_MoveDirection_Left:
		MovePlayer_helper(OrthogonalDirection_Left);
		goto main_1A64;
	case MoveDirection_UpLeft:
		MovePlayer_helper(OrthogonalDirection_Left);
		if (!IsDiagonalDoubledPhase(currentObject->y))
			goto case_MoveDirection_Up;
		if (MovePlayer_helper(OrthogonalDirection_Left))
			goto case_MoveDirection_Up;
		if (!MovePlayer_helper(OrthogonalDirection_Up))
			goto main_1A64;
		// fall through
	case MoveDirection_Right:
	case_MoveDirection_Right:
		MovePlayer_helper(OrthogonalDirection_Right);
		goto main_1A64;
	case MoveDirection_Up:
	case_MoveDirection_Up:
		MovePlayer_helper(OrthogonalDirection_Up);
	main_1A64:
		if (!data_CBF)
			PlotObjectToMaze();
		player.moveDirection = moveDirection;
		return !data_CBF;
	default:
		__assume(0);
	}
}

bool UpdatePlayer(bool playbackMode, BYTE &replayIO) // returns true if the match has been lost
{
	if (!playbackMode)
		replayIO = 0;
	currentObject = &player;
	if (++data_C74 > 7)
	{
		data_C74 = 0;
		playerAnimFrame ^= true;
	}
	if (playerAnimFrame)
		currentSprite = data_10E2;
	else
		currentSprite = data_10EC;
	if (isPlayerDying)
	{
		keyboard_state = PollKeyboard();
		if (numPlayerDeaths >= numLives)
		{
			EraseBottomTwoLines();
			CloseDirectConsole(WINDOW_HEIGHT-2);
			WriteTextToConsole("The SNIPES have triumphed!!!\r\n");
			return true;
		}
		if (!isPlayerExploding)
		{
			keyboard_state = PollKeyboard();
			isPlayerDying = false;
			PlaceObjectInRandomUnoccupiedMazeCell();
			viewportFocusX = player.x;
			viewportFocusY = player.y;
			PlotObjectToMaze();
			return false;
		}
		return false;
	}
	else
	{
		if (--player.inputFrame)
		{
			if (IsObjectTaggedToExplode())
				goto explode_player;
			return false;
		}
		keyboard_state = PollKeyboard();
		player.inputFrame = 2;
		if (IsObjectTaggedToExplode())
			goto explode_player;
	}
	EraseObjectFromMaze();
	static const BYTE arrowKeyMaskToDirectionTable[] =
	{
		0,
		MoveDirection_Right,
		MoveDirection_Left,
		0,
		MoveDirection_Down,
		MoveDirection_DownRight,
		MoveDirection_DownLeft,
		0,
		MoveDirection_Up,
		MoveDirection_UpRight,
		MoveDirection_UpLeft,
		0,
		0,
		0,
		0,
		0,
	};
	BYTE moveDirection;
	if (playbackMode)
	{
		spacebar_state = replayIO >> 7;
		moveDirection = (replayIO & 0x7F) % 9;
		if (moveDirection)
		{
			moveDirection--;
			goto playback_move;
		}
	}
	else
	if (BYTE keyboardMove = keyboard_state & (KEYSTATE_MOVE_RIGHT | KEYSTATE_MOVE_LEFT | KEYSTATE_MOVE_DOWN | KEYSTATE_MOVE_UP))
	{
		moveDirection = arrowKeyMaskToDirectionTable[keyboardMove];
playback_move:
		player.moveDirection = moveDirection;
		if (!playbackMode)
			replayIO = moveDirection + 1;
		if (MovePlayer())
		{
			if (!spacebar_state)
				goto main_1B8F;
			if (!playbackMode)
				replayIO += 0x80;
			if (player.inputFrame == 1)
			{
				EraseObjectFromMaze();
				if (!MovePlayer())
				{
					if (enableElectricWalls)
						goto explode_player;
					PlotObjectToMaze();
				}
			}
			player.inputFrame = 1;
			goto main_1B8F;
		}
		if (enableElectricWalls)
			goto explode_player;
	}
	PlotObjectToMaze();
main_1B8F:
	BYTE fireDirection;
	if (playbackMode)
	{
		fireDirection = (replayIO & 0x7F) / 9 % 9;
		if (fireDirection)
		{
			fireDirection--;
			goto playback_fire;
		}
	}
	else
	if (!spacebar_state && (keyboard_state & (KEYSTATE_FIRE_RIGHT | KEYSTATE_FIRE_LEFT | KEYSTATE_FIRE_DOWN | KEYSTATE_FIRE_UP)))
	{
		fireDirection = arrowKeyMaskToDirectionTable[keyboard_state >> 4];
playback_fire:
		if (!playbackMode)
			replayIO += (fireDirection + 1) * 9;
		if (--player.firingFrame)
			return false;
		MoveDirection moveDirection_backup = player.moveDirection;
		player.moveDirection = fireDirection;
		FireBullet(BulletType_Player);
		SetSoundEffectState(0, SoundEffect_PlayerBullet);
		player.moveDirection = moveDirection_backup;
		player.firingFrame = player.inputFrame == 1 ? playerFiringPeriod<<1 : playerFiringPeriod;
		return false;
	}
	player.firingFrame = 1;
	return false;
explode_player:
	FreeObjectInList(&player.next, OBJECT_PLAYER); // explode the player
	isPlayerDying = true;
	numPlayerDeaths++;
	return false;
}

void UpdateExplosions()
{
	for (BYTE object = objectHead_explosions; object;)
	{
		currentObject = &objects[object];
		Explosion &explosion = *(Explosion*)currentObject;
		currentSprite = FakePointerToPointer(explosion.sprite);
		EraseObjectFromMaze();
		BYTE animFrame = (explosion.animFrame + 1) % 6;
		BYTE nextObject = explosion.next;
		explosion.animFrame++;
		if (explosion.animFrame > (object == OBJECT_PLAYEREXPLOSION ? 11 : 5))
		{
			FreeObjectInList_worker(&objectHead_explosions, object);
			if (object != OBJECT_PLAYEREXPLOSION)
				FreeObject(object);
			else
				isPlayerExploding = false;
		}
		else
		{
			if (explosion.spriteSize == EXPLOSION_SIZE(2,2))
			{
				explosion.sprite = PointerToFakePointer(data_12FE[animFrame]);
				currentSprite = data_12FE[animFrame];
				if (object == OBJECT_PLAYEREXPLOSION && explosion.animFrame >= 6)
					SetSoundEffectState(5 - (explosion.animFrame - 6), SoundEffect_ExplodePlayer);
				else
					SetSoundEffectState(animFrame, SoundEffect_ExplodePlayer);
			}
			else
			if (explosion.spriteSize == EXPLOSION_SIZE(2,1))
			{
				explosion.sprite = PointerToFakePointer(data_1352[animFrame]);
				currentSprite = data_1352[animFrame];
				SetSoundEffectState(animFrame, SoundEffect_ExplodeSnipe);
			}
			else
			if (explosion.spriteSize == EXPLOSION_SIZE(1,1))
			{
				explosion.sprite = PointerToFakePointer(data_1392[animFrame]);
				currentSprite = data_1392[animFrame];
				SetSoundEffectState(animFrame, SoundEffect_ExplodeGhost);
			}
			PlotObjectToMaze();
		}
		object = nextObject;
	}
}

static const WORD sound_ExplodeGhost [] = { 100,  100, 1400, 1800, 1600, 1200};
static const WORD sound_ExplodeSnipe [] = {2200, 6600, 1800, 4400, 8400, 1100};
static const WORD sound_ExplodePlayer[] = {2000, 8000, 6500, 4000, 2500, 1000};

void UpdateSound()
{
	if (!sound_enabled)
	{
		ClearSound();
		return;
	}
	if (currentSoundEffect == SoundEffect_None)
	{
#ifdef PLAY_SOUND_DURING_SILENCE
		PlayTone(0);
#else
		PlayTone(-1);
#endif
		return;
	}
	switch (currentSoundEffect)
	{
	case SoundEffect_PlayerBullet:
		if (!currentSoundEffectFrame)
			PlayTone(1900);
		else
			PlayTone(1400);
		break;
	case SoundEffect_SnipeBullet:
		PlayTone(1600);
		break;
	case SoundEffect_ExplodeGhost:
		PlayTone(sound_ExplodeGhost[currentSoundEffectFrame]);
		break;
	case SoundEffect_ExplodeSnipe:
		PlayTone(sound_ExplodeSnipe[currentSoundEffectFrame]);
		break;
	case SoundEffect_ExplodePlayer:
		PlayTone(sound_ExplodePlayer[currentSoundEffectFrame]);
		break;
	default:
		__assume(0);
	}
	currentSoundEffect = SoundEffect_None;
}

void DrawViewport()
{
	WORD outputRow = VIEWPORT_ROW;
	SHORT data_298 = viewportFocusY - VIEWPORT_HEIGHT / 2;
	if (data_298 < 0)
		data_298 += MAZE_HEIGHT;
	SHORT data_296 = viewportFocusX - WINDOW_WIDTH / 2;
	if (data_296 < 0)
		data_296 += MAZE_WIDTH;
	SHORT wrappingColumn = 0;
	if (data_296 + WINDOW_WIDTH >= MAZE_WIDTH)
		wrappingColumn = MAZE_WIDTH - data_296;
	else
		wrappingColumn = WINDOW_WIDTH;
	for (Uint row = 0; row < VIEWPORT_HEIGHT; row++)
	{
		WORD data_29C = data_298 * MAZE_WIDTH;
		WriteTextMem(wrappingColumn, outputRow, 0, &maze[data_296 + data_29C]);
		if (wrappingColumn != WINDOW_WIDTH)
			WriteTextMem(WINDOW_WIDTH - wrappingColumn, outputRow, wrappingColumn, &maze[data_29C]);
		outputRow++;
		if (++data_298 == MAZE_HEIGHT)
			data_298 = 0;
	}
}

int __cdecl main(int argc, char* argv[])
{
	if (argc > 2)
	{
		fprintf(stderr, "Usage: %s [filename of replay to play back]\n", argv[0]);
		return -1;
	}
	bool playbackMode = argc == 2;

	if (int result = OpenConsole())
		return result;
	if (int result = OpenTimer())
	{
		CloseConsole();
		return result;
	}
	if (int result = OpenSound())
	{
		CloseConsole();
		CloseTimer();
		return result;
	}

	ClearConsole();
	if (!playbackMode)
	{
		SetConsoleOutputTextColor(FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED);
#define _ " "
#define S "\x01"
#define i "\x18"
#define TITLE_SCREEN \
		"       ported by David Ellsworth\r\n"\
		"\r\n"\
		_ _ _ _ i i i _ _ i _ _ _ i _ i i i _ i i i i _ _ i i i i i _ _ i i i "\r\n"\
		_ _ _ i S S S i _ S _ _ _ S _ S S S _ S S S S i _ S S S S S _ i S S S i "\r\n"\
		_ _ _ S _ _ _ S _ i i _ _ i _ _ i _ _ i _ _ _ S _ i _ _ _ _ _ S _ _ _ S "\r\n"\
		_ _ _ i _ _ _ _ _ S S _ _ S _ _ S _ _ S _ _ _ i _ S _ _ _ _ _ i "\r\n"\
		_ _ _ S i i i _ _ i _ i _ i _ _ i _ _ i i i i S _ i i i i _ _ S i i i "\r\n"\
		_ _ _ _ S S S i _ S _ S _ S _ _ S _ _ S S S S _ _ S S S S _ _ _ S S S i "\r\n"\
		_ _ _ _ _ _ _ S _ i _ _ i i _ _ i _ _ i _ _ _ _ _ i _ _ _ _ _ _ _ _ _ S "\r\n"\
		_ _ _ i _ _ _ i _ S _ _ S S _ _ S _ _ S _ _ _ _ _ S _ _ _ _ _ i _ _ _ i "\r\n"\
		_ _ _ S i i i S _ i _ _ _ i _ i i i _ i _ _ _ _ _ i i i i i _ S i i i S "\r\n"\
		_ _ _ _ S S S _ _ S _ _ _ S _ S S S _ S _ _ _ _ _ S S S S S _ _ S S S "\r\n"\
		"\r\n\r\n"\
		"(c)Copyright SuperSet Software Corp 1982\r\n"\
		"All Rights Reserved\r\n"\
		"\r\n\r\n"
		WriteTextToConsole(TITLE_SCREEN "Enter skill level (A-Z)(1-9): ");
#undef _
#undef S
#undef i
		ReadSkillLevel();
		if (got_ctrl_break)
			goto do_not_play_again;
	}

	WORD tick_count = GetTickCountWord();
	random_seed_lo = (BYTE)tick_count;
	if (!random_seed_lo)
		random_seed_lo = 444;
	random_seed_hi = tick_count >> 8;
	if (!random_seed_hi)
		random_seed_hi = 555;

	for (;;)
	{
		FILE *replayFile = NULL;
		if (playbackMode)
		{
			replayFile = fopen(argv[1], "rb");
			if (!replayFile)
			{
				fprintf(stderr, "Error opening replay file \"%s\" for playback\n", argv[1]);
				return -1;
			}
			fread(&random_seed_lo, sizeof(random_seed_lo), 1, replayFile);
			fread(&random_seed_hi, sizeof(random_seed_hi), 1, replayFile);
			fread(&skillLevelLetter, 1, 1, replayFile);
			fread(&skillLevelNumber, 1, 1, replayFile);
		}
		else
		{
			time_t rectime = time(NULL);
			struct tm *rectime_gmt;
			rectime_gmt = gmtime(&rectime);

			char replayFilename[MAX_PATH];
			sprintf(replayFilename,
					"%04d-%02d-%02d %02d.%02d.%02d.SnipesGame",
					1900+rectime_gmt->tm_year, rectime_gmt->tm_mon+1, rectime_gmt->tm_mday,
					rectime_gmt->tm_hour, rectime_gmt->tm_min, rectime_gmt->tm_sec);

			replayFile = fopen(replayFilename, "wb");
			if (replayFile)
			{
				setvbuf(replayFile, NULL, _IOFBF, 64);
				fwrite(&random_seed_lo, sizeof(random_seed_lo), 1, replayFile);
				fwrite(&random_seed_hi, sizeof(random_seed_hi), 1, replayFile);
				fwrite(&skillLevelLetter, 1, 1, replayFile);
				fwrite(&skillLevelNumber, 1, 1, replayFile);
			}
		}

		enableElectricWalls          = skillLevelLetter >= 'M'-'A';
		generatorsResistSnipeBullets = skillLevelLetter >= 'W'-'A';
		enableRubberBullets          = rubberBulletTable         [skillLevelLetter];
		snipeShootingAccuracy        = snipeShootingAccuracyTable[skillLevelLetter];
		enableGhostSnipes            = enableGhostSnipesTable    [skillLevelLetter];
		ghostBitingAccuracy          = ghostBitingAccuracyTable  [skillLevelLetter];
		maxSnipes                    = maxSnipesTable            [skillLevelNumber-1];
		numGeneratorsAtStart         = numGeneratorsTable        [skillLevelNumber-1];
		numLives                     = numLivesTable             [skillLevelNumber-1];
		playerFiringPeriod           = 2;

		OpenDirectConsole();
		if (int result = OpenKeyboard())
		{
			CloseConsole();
			CloseTimer();
			CloseSound();
			return result;
		}

		frame = 0;
		OutputHUD();
		CreateMaze();
		CreateGeneratorsAndPlayer();
		SetSoundEffectState(0, SoundEffect_None);

		for (;;)
		{
			if (forfeit_match)
			{
				EraseBottomTwoLines();
				break;
			}
			if (UpdateHUD())
				break;

#ifdef CHEAT_FAST_FORWARD
			if (!fast_forward)
#else
			if (!playbackMode || !fast_forward)
#endif
			{
				for (;;)
				{
					Sleep(1);
					WORD tick_count2 = GetTickCountWord();
					if (tick_count2 != tick_count)
					{
						tick_count = tick_count2;
						break;
					}
				}
			}

			UpdateBullets();
			UpdateGhosts();
			UpdateSnipes();
			UpdateGenerators();

			BYTE replayIO;
			bool playbackFinished = false;
			if (playbackMode && fread(&replayIO, 1, 1, replayFile) == 0)
				playbackFinished = true;
			if (UpdatePlayer(playbackMode, replayIO) || playbackFinished)
				break;
			if (!playbackMode && replayFile)
				fwrite(&replayIO, 1, 1, replayFile);

			UpdateExplosions();
			UpdateSound();
			DrawViewport();
		}

		ClearSound();
		forfeit_match = false;

		if (replayFile)
			fclose(replayFile);

		CloseDirectConsole(WINDOW_HEIGHT-1 - (playbackMode ? 1 : 0));
		if (playbackMode)
			break;
		for (;;)
		{
			WriteTextToConsole("Play another game? (Y or N) ");
			char playAgain;
			ReadTextFromConsole(&playAgain, 1);
			if (got_ctrl_break)
				goto do_not_play_again;
			if (playAgain == 'Y' || playAgain == 'y')
				goto do_play_again;
			if (playAgain == 'N' || playAgain == 'n')
				goto do_not_play_again;
		}
	do_play_again:
		WriteTextToConsole("Enter new skill level (A-Z)(1-9): ");
		ReadSkillLevel();
		if (got_ctrl_break)
			goto do_not_play_again;
	}
do_not_play_again:

	CloseSound();
	CloseConsole();

	//timeEndPeriod(1);

	return 0;
}
