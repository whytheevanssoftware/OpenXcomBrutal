/*
 * Copyright 2010-2016 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <climits>
#include <algorithm>
#include "AIModule.h"
#include "../Savegame/BattleItem.h"
#include "../Savegame/Node.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Savegame/SavedGame.h"
#include "TileEngine.h"
#include "BattlescapeState.h"
#include "../Savegame/Tile.h"
#include "Pathfinding.h"
#include "../Engine/RNG.h"
#include "../Engine/Logger.h"
#include "../Engine/Game.h"
#include "../Mod/Armor.h"
#include "../Mod/Mod.h"
#include "../Mod/RuleItem.h"
#include "../Mod/RuleSoldierBonus.h"
#include "../fmath.h"

namespace OpenXcom
{


/**
 * Sets up a BattleAIState.
 * @param save Pointer to the battle game.
 * @param unit Pointer to the unit.
 * @param node Pointer to the node the unit originates from.
 */
AIModule::AIModule(SavedBattleGame *save, BattleUnit *unit, Node *node) :
	_save(save), _unit(unit), _aggroTarget(0), _knownEnemies(0), _visibleEnemies(0), _spottingEnemies(0),
    _escapeTUs(0), _ambushTUs(0), _weaponPickedUp(false), _wantToEndTurn(false), _rifle(false), _melee(false), _blaster(false), _grenade(false), _ranOutOfTUs(false),
	_didPsi(false), _AIMode(AI_PATROL), _closestDist(100), _fromNode(node), _toNode(0), _foundBaseModuleToDestroy(false)
{
	_traceAI = Options::traceAI;

	_reserve = BA_NONE;
	_intelligence = _unit->getIntelligence();
	_escapeAction = BattleAction();
	_ambushAction = BattleAction();
	_attackAction = BattleAction();
	_patrolAction = BattleAction();
	_psiAction = BattleAction();
	_targetFaction = FACTION_PLAYER;
	_myFaction = _unit->getOriginalFaction();
	if (_unit->getOriginalFaction() == FACTION_NEUTRAL || _unit->getOriginalFaction() == FACTION_PLAYER)
	{
		_targetFaction = FACTION_HOSTILE;
	}
}

/**
 * Deletes the BattleAIState.
 */
AIModule::~AIModule()
{

}

/**
 * Resets the unsaved AI state.
 */
void AIModule::reset()
{
	// these variables are not saved in save() and also not initiated in think()
	_escapeTUs = 0;
	_ambushTUs = 0;
}

/**
 * Loads the AI state from a YAML file.
 * @param node YAML node.
 */
void AIModule::load(const YAML::Node &node)
{
	int fromNodeID, toNodeID;
	fromNodeID = node["fromNode"].as<int>(-1);
	toNodeID = node["toNode"].as<int>(-1);
	_AIMode = node["AIMode"].as<int>(AI_PATROL);
	_wasHitBy = node["wasHitBy"].as<std::vector<int> >(_wasHitBy);
	_weaponPickedUp = node["weaponPickedUp"].as<bool>(_weaponPickedUp);
	// TODO: Figure out why AI are sometimes left with junk nodes
	if (fromNodeID >= 0 && (size_t)fromNodeID < _save->getNodes()->size())
	{
		_fromNode = _save->getNodes()->at(fromNodeID);
	}
	if (toNodeID >= 0 && (size_t)toNodeID < _save->getNodes()->size())
	{
		_toNode = _save->getNodes()->at(toNodeID);
	}
}

/**
 * Saves the AI state to a YAML file.
 * @return YAML node.
 */
YAML::Node AIModule::save() const
{
	int fromNodeID = -1, toNodeID = -1;
	if (_fromNode)
		fromNodeID = _fromNode->getID();
	if (_toNode)
		toNodeID = _toNode->getID();

	YAML::Node node;
	node.SetStyle(YAML::EmitterStyle::Flow);
	node["fromNode"] = fromNodeID;
	node["toNode"] = toNodeID;
	node["AIMode"] = _AIMode;
	node["wasHitBy"] = _wasHitBy;
	if (_weaponPickedUp)
		node["weaponPickedUp"] = _weaponPickedUp;
	return node;
}

/**
 * Mindless charge strategy. For mindless units.
 * Consists of running around and charging nearest visible enemy.
 * @param action (possible) AI action to execute after thinking is done.
 */
void AIModule::dont_think(BattleAction *action)
{
	_melee = false;
	action->weapon = _unit->getUtilityWeapon(BT_MELEE);

	if (_traceAI)
	{
		Log(LOG_INFO) << "LEEROY: Unit " << _unit->getId() << " of type " << _unit->getType() << " is Leeroy...";
	}
	if (action->weapon)
	{
		if (action->weapon->getRules()->getBattleType() == BT_MELEE)
		{
			if (_save->canUseWeapon(action->weapon, _unit, false, BA_HIT))
			{
				_melee = true;
			}
		}
		else
		{
			action->weapon = 0;
		}
	}

	auto canRun = _melee && _unit->getArmor()->allowsRunning(false) && _unit->getEnergy() > _unit->getBaseStats()->stamina * 0.4f;
	int visibleEnemiesToAttack = selectNearestTargetLeeroy(canRun);
	if (_traceAI)
	{
		Log(LOG_INFO) << "LEEROY: visibleEnemiesToAttack: " << visibleEnemiesToAttack << " _melee: " << _melee << (canRun ? " run" : "");
	}
	if ((visibleEnemiesToAttack > 0) && _melee)
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "LEEROY: LEEROYIN' at someone!";
		}
		meleeActionLeeroy(canRun);
		action->type = _attackAction.type;
		action->run = _attackAction.run;
		action->target = _attackAction.target;
		// if this is a firepoint action, set our facing.
		action->finalFacing = _attackAction.finalFacing;
		action->updateTU();
	}
	else
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "LEEROY: No one to LEEROY!, patrolling...";
		}
		setupPatrol();
		_unit->setCharging(0);
		_reserve = BA_NONE;
		action->type = _patrolAction.type;
		action->target = _patrolAction.target;
	}
}

/**
 * Runs any code the state needs to keep updating every AI cycle.
 * @param action (possible) AI action to execute after thinking is done.
 */
void AIModule::think(BattleAction *action)
{
	action->type = BA_RETHINK;
	action->actor = _unit;
	action->weapon = _unit->getMainHandWeapon(false);
	_attackAction.diff = _save->getBattleState()->getGame()->getSavedGame()->getDifficultyCoefficient();
	_attackAction.actor = _unit;
	_attackAction.run = false;
	_attackAction.weapon = action->weapon;
	_attackAction.number = action->number;
	_escapeAction.number = action->number;
	_knownEnemies = countKnownTargets();
	_visibleEnemies = selectNearestTarget();
	_spottingEnemies = getSpottingUnits(_unit->getPosition());
	_melee = (_unit->getUtilityWeapon(BT_MELEE) != 0);
	_rifle = false;
	_blaster = false;
	_ranOutOfTUs = false;
	_reachable = _save->getPathfinding()->findReachable(_unit, BattleActionCost(), _ranOutOfTUs);
	_wasHitBy.clear();
	_foundBaseModuleToDestroy = false;
	bool dummy = false;

	if (_unit->getCharging() && _unit->getCharging()->isOut())
	{
		_unit->setCharging(0);
	}

	if (_traceAI && !_unit->isBrutal())
	{
		Log(LOG_INFO) << "#" << _unit->getId() << "--" << _unit->getType();
		if (_unit->getFaction() == FACTION_HOSTILE)
		{
			Log(LOG_INFO) << "Unit has " << _visibleEnemies << "/" << _knownEnemies << " known enemies visible, " << _spottingEnemies << " of whom are spotting him. ";
		}
		else
		{
			Log(LOG_INFO) << "Civilian Unit has " << _visibleEnemies << " enemies visible, " << _spottingEnemies << " of whom are spotting him. ";
		}
		std::string AIMode;
		switch (_AIMode)
		{
		case AI_PATROL:
			AIMode = "Patrol";
			break;
		case AI_AMBUSH:
			AIMode = "Ambush";
			break;
		case AI_COMBAT:
			AIMode = "Combat";
			break;
		case AI_ESCAPE:
			AIMode = "Escape";
			break;
		}
		Log(LOG_INFO) << "Currently using " << AIMode << " behaviour";
	}

	// Brutal gets priority over Leeroy
	if (_unit->isLeeroyJenkins() && !_unit->isBrutal())
	{
		dont_think(action);
		return;
	}

	Mod *mod = _save->getBattleState()->getGame()->getMod();
	if (action->weapon)
	{
		const RuleItem *rule = action->weapon->getRules();
		if (_save->canUseWeapon(action->weapon, _unit, false, BA_NONE)) // Note: ammo is not checked here
		{
			if (rule->getBattleType() == BT_FIREARM)
			{
				if (action->weapon->getCurrentWaypoints() != 0)
				{
					_blaster = true;
					if (!_unit->isBrutal())
						_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_AIMEDSHOT, _unit, action->weapon), dummy);
				}
				else
				{
					_rifle = true;
					if (!_unit->isBrutal())
						_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_SNAPSHOT, _unit, action->weapon), dummy);
				}
			}
			else if (rule->getBattleType() == BT_MELEE)
			{
				_melee = true;
				if (!_unit->isBrutal())
					_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_HIT, _unit, action->weapon), dummy);
			}
		}
		else
		{
			action->weapon = 0;
		}
	}

	BattleItem *grenade = _unit->getGrenadeFromBelt();
	_grenade = grenade != 0 && _save->getTurn() >= grenade->getRules()->getAIUseDelay(mod);

	if (_unit->isBrutal())
	{
		brutalThink(action);
		return;
	}
	
	if (_unit->isXCOMmandAI())
	{
		xcommandAIthink(action);
		return;
	}

	if (_spottingEnemies && !_escapeTUs)
	{
		setupEscape();
	}

	if (_knownEnemies && !_melee && !_ambushTUs)
	{
		setupAmbush();
	}

	setupAttack();
	setupPatrol();

	if (_psiAction.type != BA_NONE && !_didPsi && _save->getTurn() >= _psiAction.weapon->getRules()->getAIUseDelay(mod))
	{
		_didPsi = true;
		action->type = _psiAction.type;
		action->target = _psiAction.target;
		action->number -= 1;
		action->weapon = _psiAction.weapon;
		action->updateTU();
		return;
	}
	else
	{
		_didPsi = false;
	}

	bool evaluate = false;

	switch (_AIMode)
		{
		case AI_PATROL:
			evaluate = (bool)(_spottingEnemies || _visibleEnemies || _knownEnemies || RNG::percent(10));
			break;
		case AI_AMBUSH:
			evaluate = (!_rifle || !_ambushTUs || _visibleEnemies);
			break;
		case AI_COMBAT:
			evaluate = (_attackAction.type == BA_RETHINK);
			break;
		case AI_ESCAPE:
			evaluate = (!_spottingEnemies || !_knownEnemies);
			break;
			}

	if (_weaponPickedUp)
	{
		evaluate = true;
		_weaponPickedUp = false;
	}
	else if (_spottingEnemies > 2
		|| _unit->getHealth() < 2 * _unit->getBaseStats()->health / 3)
	{
		evaluate = true;
	}
	else if (_aggroTarget && _aggroTarget->getTurnsSinceSpotted() > _intelligence)
	{
		// Special case for snipers, target may not be visible, but that shouldn't cause us to re-evaluate
		if (!_unit->isSniper() || !_aggroTarget->getTurnsLeftSpottedForSnipers())
		{
			evaluate = true;
		}
	}


	if (_save->isCheating() && _AIMode != AI_COMBAT)
	{
		evaluate = true;
	}

	if (evaluate)
	{
		evaluateAIMode();
		if (_traceAI)
		{
			std::string AIMode;
			switch (_AIMode)
			{
			case AI_PATROL:
				AIMode = "Patrol";
				break;
			case AI_AMBUSH:
				AIMode = "Ambush";
				break;
			case AI_COMBAT:
				AIMode = "Combat";
				break;
			case AI_ESCAPE:
				AIMode = "Escape";
				break;
			}
			Log(LOG_INFO) << "Re-Evaluated, now using " << AIMode << " behaviour";
		}
	}

	_reserve = BA_NONE;

	switch (_AIMode)
	{
	case AI_ESCAPE:
		_unit->setCharging(0);
		action->type = _escapeAction.type;
		action->target = _escapeAction.target;
		// end this unit's turn.
		action->finalAction = true;
		// ignore new targets.
		action->desperate = true;
		// if armor allow runing then run way from there.
		action->run = _escapeAction.run;
		// spin 180 at the end of your route.
		_unit->setHiding(true);
		break;
	case AI_PATROL:
		_unit->setCharging(0);
		if (action->weapon && action->weapon->getRules()->getBattleType() == BT_FIREARM)
		{
			switch (_unit->getAggression())
			{
			case 0:
				_reserve = BA_AIMEDSHOT;
				break;
			case 1:
				_reserve = BA_AUTOSHOT;
				break;
			case 2:
				_reserve = BA_SNAPSHOT;
				break;
			default:
				break;
			}
		}
		action->type = _patrolAction.type;
		action->target = _patrolAction.target;
		break;
	case AI_COMBAT:
		action->type = _attackAction.type;
		action->target = _attackAction.target;
		// this may have changed to a grenade.
		action->weapon = _attackAction.weapon;
		if (action->weapon && action->type == BA_THROW && action->weapon->getRules()->getBattleType() == BT_GRENADE)
		{
			_unit->spendCost(_unit->getActionTUs(BA_PRIME, action->weapon));
			_unit->spendTimeUnits(4);
		}
		// if this is a firepoint action, set our facing.
		action->finalFacing = _attackAction.finalFacing;
		action->updateTU();
		// if this is a "find fire point" action, don't increment the AI counter.
		if (action->type == BA_WALK && _rifle && _unit->getArmor()->allowsMoving()
			// so long as we can take a shot afterwards.
			&& BattleActionCost(BA_SNAPSHOT, _unit, action->weapon).haveTU())
		{
			action->number -= 1;
		}
		else if (action->type == BA_LAUNCH)
		{
			action->waypoints = _attackAction.waypoints;
		}
		else if (action->type == BA_AIMEDSHOT || action->type == BA_AUTOSHOT)
		{
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
		break;
	case AI_AMBUSH:
		_unit->setCharging(0);
		action->type = _ambushAction.type;
		action->target = _ambushAction.target;
		// face where we think our target will appear.
		action->finalFacing = _ambushAction.finalFacing;
		// end this unit's turn.
		action->finalAction = true;
		action->kneel = _unit->getArmor()->allowsKneeling(false);
		break;
	default:
		break;
	}

	if (action->type == BA_WALK)
	{
		// if we're moving, we'll have to re-evaluate our escape/ambush position.
		if (action->target != _unit->getPosition())
		{
			_escapeTUs = 0;
			_ambushTUs = 0;
		}
		else
		{
			action->type = BA_NONE;
		}
	}
}


/*
 * sets the "was hit" flag to true.
 */
void AIModule::setWasHitBy(BattleUnit *attacker)
{
	if (attacker->getFaction() != _unit->getFaction() && !getWasHitBy(attacker->getId()))
		_wasHitBy.push_back(attacker->getId());
}

/*
 * Sets the "unit picked up a weapon" flag.
 */
void AIModule::setWeaponPickedUp()
{
	_weaponPickedUp = true;
}

/*
 * Gets whether the unit was hit.
 * @return if it was hit.
 */
bool AIModule::getWasHitBy(int attacker) const
{
	return std::find(_wasHitBy.begin(), _wasHitBy.end(), attacker) != _wasHitBy.end();
}
/*
 * Sets up a patrol action.
 * this is mainly going from node to node, moving about the map.
 * handles node selection, and fills out the _patrolAction with useful data.
 */
void AIModule::setupPatrol()
{
	_patrolAction.clearTU();
	if (_toNode != 0 && _unit->getPosition() == _toNode->getPosition())
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Patrol destination reached!";
		}
		// destination reached
		// head off to next patrol node
		_fromNode = _toNode;
		freePatrolTarget();
		_toNode = 0;
		// take a peek through window before walking to the next node
		int dir = _save->getTileEngine()->faceWindow(_unit->getPosition());
		if (dir != -1 && dir != _unit->getDirection())
		{
			_unit->lookAt(dir);
			while (_unit->getStatus() == STATUS_TURNING)
			{
				_unit->turn();
			}
		}
	}

	if (_fromNode == 0)
	{
		// assume closest node as "from node"
		// on same level to avoid strange things, and the node has to match unit size or it will freeze
		int closest = 1000000;
		for (auto* node : *_save->getNodes())
		{
			if (node->isDummy())
			{
				continue;
			}
			int d = Position::distanceSq(_unit->getPosition(), node->getPosition());
			if (_unit->getPosition().z == node->getPosition().z
				&& d < closest
				&& (!(node->getType() & Node::TYPE_SMALL) || _unit->getArmor()->getSize() == 1))
			{
				_fromNode = node;
				closest = d;
			}
		}
	}
	int triesLeft = 5;

	while (_toNode == 0 && triesLeft)
	{
		triesLeft--;
		// look for a new node to walk towards
		bool scout = true;
		if (_save->getMissionType() != "STR_BASE_DEFENSE")
		{
			// after turn 20 or if the morale is low, everyone moves out the UFO and scout
			// also anyone standing in fire should also probably move
			if (_save->isCheating() || !_fromNode || _fromNode->getRank() == 0 ||
				(_save->getTile(_unit->getPosition()) && _save->getTile(_unit->getPosition())->getFire()))
			{
				scout = true;
			}
			else
			{
				scout = false;
			}
		}

		// in base defense missions, the smaller aliens walk towards target nodes - or if there, shoot objects around them
		else if (_unit->getArmor()->getSize() == 1 && _unit->getOriginalFaction() == FACTION_HOSTILE)
		{
			// can i shoot an object?
			if (_fromNode->isTarget() &&
				_attackAction.weapon &&
				_attackAction.weapon->getRules()->getAccuracySnap() &&
				_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT) &&
				_attackAction.weapon->getAmmoForAction(BA_SNAPSHOT)->getRules()->getDamageType()->isDirect() &&
				_save->canUseWeapon(_attackAction.weapon, _unit, false, BA_SNAPSHOT) &&
				_save->getModuleMap()[_fromNode->getPosition().x / 10][_fromNode->getPosition().y / 10].second > 0)
			{
				// scan this room for objects to destroy
				int x = (_unit->getPosition().x/10)*10;
				int y = (_unit->getPosition().y/10)*10;
				for (int i = x; i < x+9; i++)
				for (int j = y; j < y+9; j++)
				{
					MapData *md = _save->getTile(Position(i, j, 1))->getMapData(O_OBJECT);
					if (md && md->isBaseModule())
					{
						_patrolAction.actor = _unit;
						_patrolAction.target = Position(i, j, 1);
						_patrolAction.weapon = _attackAction.weapon;
						_patrolAction.type = BA_SNAPSHOT;
						_patrolAction.updateTU();
						_foundBaseModuleToDestroy = _save->getBattleGame()->getMod()->getAIDestroyBaseFacilities();
						return;
					}
				}
			}
			else
			{
				// find closest high value target which is not already allocated
				int closest = 1000000;
				for (auto* node : *_save->getNodes())
				{
					if (node->isDummy())
					{
						continue;
					}
					if (node->isTarget() && !node->isAllocated())
					{
						int d = Position::distanceSq(_unit->getPosition(), node->getPosition());
						if (!_toNode ||  (d < closest && node != _fromNode))
						{
							_toNode = node;
							closest = d;
						}
					}
				}
			}
		}

		if (_toNode == 0)
		{
			_toNode = _save->getPatrolNode(scout, _unit, _fromNode);
			if (_toNode == 0)
			{
				_toNode = _save->getPatrolNode(!scout, _unit, _fromNode);
			}
		}

		if (_toNode != 0)
		{
			_save->getPathfinding()->calculate(_unit, _toNode->getPosition(), BAM_NORMAL);
			if (_save->getPathfinding()->getStartDirection() == -1)
			{
				_toNode = 0;
			}
			_save->getPathfinding()->abortPath();
		}
	}

	if (_toNode != 0)
	{
		_toNode->allocateNode();
		_patrolAction.actor = _unit;
		_patrolAction.type = BA_WALK;
		_patrolAction.target = _toNode->getPosition();
	}
	else
	{
		_patrolAction.type = BA_RETHINK;
	}
}

/**
 * Try to set up an ambush action
 * The idea is to check within a 11x11 tile square for a tile which is not seen by our aggroTarget,
 * but that can be reached by him. we then intuit where we will see the target first from our covered
 * position, and set that as our final facing.
 * Fills out the _ambushAction with useful data.
 */
void AIModule::setupAmbush()
{
	_ambushAction.type = BA_RETHINK;
	int bestScore = 0;
	_ambushTUs = 0;
	std::vector<int> path;

	if (selectClosestKnownEnemy())
	{
		const int BASE_SYSTEMATIC_SUCCESS = 100;
		const int COVER_BONUS = 25;
		const int FAST_PASS_THRESHOLD = 80;
		Position origin = _save->getTileEngine()->getSightOriginVoxel(_aggroTarget);

		// we'll use node positions for this, as it gives map makers a good degree of control over how the units will use the environment.
		for (const auto* node : *_save->getNodes())
		{
			if (node->isDummy())
			{
				continue;
			}
			Position pos = node->getPosition();
			Tile *tile = _save->getTile(pos);
			if (tile == 0 || Position::distance2d(pos, _unit->getPosition()) > 10 || pos.z != _unit->getPosition().z || tile->getDangerous() ||
				std::find(_reachableWithAttack.begin(), _reachableWithAttack.end(), _save->getTileIndex(pos))  == _reachableWithAttack.end())
				continue; // just ignore unreachable tiles

			if (_traceAI)
			{
				// colour all the nodes in range purple.
				tile->setPreview(10);
				tile->setMarkerColor(13);
			}

			// make sure we can't be seen here.
			Position target;
			if (!_save->getTileEngine()->canTargetUnit(&origin, tile, &target, _aggroTarget, false, _unit) && !getSpottingUnits(pos))
			{
				_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
				int ambushTUs = _save->getPathfinding()->getTotalTUCost();
				// make sure we can move here
				if (_save->getPathfinding()->getStartDirection() != -1)
				{
					int score = BASE_SYSTEMATIC_SUCCESS;
					score -= ambushTUs;

					// make sure our enemy can reach here too.
					_save->getPathfinding()->calculate(_aggroTarget, pos, BAM_NORMAL);

					if (_save->getPathfinding()->getStartDirection() != -1)
					{
						// ideally we'd like to be behind some cover, like say a window or a low wall.
						if (_save->getTileEngine()->faceWindow(pos) != -1)
						{
							score += COVER_BONUS;
						}
						if (score > bestScore)
						{
							path = _save->getPathfinding()->copyPath();
							bestScore = score;
							_ambushTUs = (pos == _unit->getPosition()) ? 1 : ambushTUs;
							_ambushAction.target = pos;
							if (bestScore > FAST_PASS_THRESHOLD)
							{
								break;
							}
						}
					}
				}
			}
		}

		if (bestScore > 0)
		{
			_ambushAction.type = BA_WALK;
			// i should really make a function for this
			origin = _ambushAction.target.toVoxel() +
				// 4 because -2 is eyes and 2 below that is the rifle (or at least that's my understanding)
				Position(8,8, _unit->getHeight() + _unit->getFloatHeight() - _save->getTile(_ambushAction.target)->getTerrainLevel() - 4);
			Position currentPos = _aggroTarget->getPosition();
			_save->getPathfinding()->setUnit(_aggroTarget);
			size_t tries = path.size();
			// hypothetically walk the target through the path.
			while (tries > 0)
			{
				currentPos = _save->getPathfinding()->getTUCost(currentPos, path.back(), _aggroTarget, 0, BAM_NORMAL).pos;
				path.pop_back();
				Tile *tile = _save->getTile(currentPos);
				Position target;
				// do a virtual fire calculation
				if (_save->getTileEngine()->canTargetUnit(&origin, tile, &target, _unit, false, _aggroTarget))
				{
					// if we can virtually fire at the hypothetical target, we know which way to face.
					_ambushAction.finalFacing = _save->getTileEngine()->getDirectionTo(_ambushAction.target, currentPos);
					break;
				}
				--tries;
			}
			if (_traceAI)
			{
				Log(LOG_INFO) << "Ambush estimation will move to " << _ambushAction.target;
			}
			return;
		}
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Ambush estimation failed";
	}
}

/**
 * Try to set up a combat action
 * This will either be a psionic, grenade, or weapon attack,
 * or potentially just moving to get a line of sight to a target.
 * Fills out the _attackAction with useful data.
 */
void AIModule::setupAttack()
{
	_attackAction.type = BA_RETHINK;
	_psiAction.type = BA_NONE;

	bool sniperAttack = false;

	// if enemies are known to us but not necessarily visible, we can attack them with a blaster launcher or psi or a sniper attack.
	if (_knownEnemies)
	{
		if (psiAction())
		{
			// at this point we can save some time with other calculations - the unit WILL make a psionic attack this turn.
			return;
		}
		if (_blaster)
		{
			wayPointAction();
		}
		else if (_unit->getUnitRules()) // xcom soldiers (under mind control) lack unit rules!
		{
			// don't always act on spotter information unless modder says so
			if (RNG::percent(_unit->getUnitRules()->getSniperPercentage()))
			{
				sniperAttack = sniperAction();
			}
		}
	}

	// if we CAN see someone, that makes them a viable target for "regular" attacks.
	// This is skipped if sniperAction has already chosen an attack action
	if (!sniperAttack && selectNearestTarget())
	{
		// if we have both types of weapon, make a determination on which to use.
		if (_melee && _rifle)
		{
			selectMeleeOrRanged();
		}
		if (_grenade)
		{
			grenadeAction();
		}
		if (_melee)
		{
			meleeAction();
		}
		if (_rifle)
		{
			projectileAction();
		}
	}

	if (_attackAction.type != BA_RETHINK)
	{
		if (_traceAI)
		{
			if (_attackAction.type != BA_WALK)
			{
				Log(LOG_INFO) << "Attack estimation desires to shoot at " << _attackAction.target;
			}
			else
			{
				Log(LOG_INFO) << "Attack estimation desires to move to " << _attackAction.target;
			}
		}
		return;
	}
	else if (_spottingEnemies || _unit->getAggression() < RNG::generate(0, 3))
	{
		// if enemies can see us, or if we're feeling lucky, we can try to spot the enemy.
		if (findFirePoint())
		{
			if (_traceAI)
			{
				Log(LOG_INFO) << "Attack estimation desires to move to " << _attackAction.target;
			}
			return;
		}
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Attack estimation failed";
	}
}

/**
 * Attempts to find cover, and move toward it.
 * The idea is to check within a 11x11 tile square for a tile which is not seen by our aggroTarget.
 * If there is no such tile, we run away from the target.
 * Fills out the _escapeAction with useful data.
 */
void AIModule::setupEscape()
{
	int unitsSpottingMe = getSpottingUnits(_unit->getPosition());
	int currentTilePreference = 15;
	int tries = -1;
	bool coverFound = false;
	selectNearestTarget();
	_escapeTUs = 0;

	int dist = _aggroTarget ? Position::distance2d(_unit->getPosition(), _aggroTarget->getPosition()) : 0;

	int bestTileScore = -100000;
	int score = -100000;
	Position bestTile(0, 0, 0);
	bool run = false;

	Tile *tile = 0;

	// weights of various factors in choosing a tile to which to withdraw
	const int EXPOSURE_PENALTY = 10;
	const int FIRE_PENALTY = 40;
	const int BASE_SYSTEMATIC_SUCCESS = 100;
	const int BASE_DESPERATE_SUCCESS = 110;
	const int FAST_PASS_THRESHOLD = 100; // a score that's good enough to quit the while loop early; it's subjective, hand-tuned and may need tweaking

	std::vector<Position> randomTileSearch = _save->getTileSearch();
	RNG::shuffle(randomTileSearch);

	while (tries < 150 && !coverFound)
	{
		_escapeAction.target = _unit->getPosition(); // start looking in a direction away from the enemy
		_escapeAction.run = _unit->getArmor()->allowsRunning(false) && (tries & 1); // every odd try, i.e. roughly 50%

		if (!_save->getTile(_escapeAction.target))
		{
			_escapeAction.target = _unit->getPosition(); // cornered at the edge of the map perhaps?
		}

		score = 0;

		if (tries == -1)
		{
			// you know, maybe we should just stay where we are and not risk reaction fire...
			// or maybe continue to wherever we were running to and not risk looking stupid
			if (_save->getTile(_unit->lastCover) != 0)
			{
				_escapeAction.target = _unit->lastCover;
			}
		}
		else if (tries < 121)
		{
			// looking for cover
			_escapeAction.target.x += randomTileSearch[tries].x;
			_escapeAction.target.y += randomTileSearch[tries].y;
			score = BASE_SYSTEMATIC_SUCCESS;
			if (_escapeAction.target == _unit->getPosition())
			{
				if (unitsSpottingMe > 0)
				{
					// maybe don't stay in the same spot? move or something if there's any point to it?
					_escapeAction.target.x += RNG::generate(-20,20);
					_escapeAction.target.y += RNG::generate(-20,20);
				}
				else
				{
					score += currentTilePreference;
				}
			}
		}
		else
		{
			if (tries == 121)
			{
				if (_traceAI)
				{
					Log(LOG_INFO) << "best score after systematic search was: " << bestTileScore;
				}
			}

			score = BASE_DESPERATE_SUCCESS; // ruuuuuuun
			_escapeAction.target = _unit->getPosition();
			_escapeAction.target.x += RNG::generate(-10,10);
			_escapeAction.target.y += RNG::generate(-10,10);
			_escapeAction.target.z = _unit->getPosition().z + RNG::generate(-1,1);
			if (_escapeAction.target.z < 0)
			{
				_escapeAction.target.z = 0;
			}
			else if (_escapeAction.target.z >= _save->getMapSizeZ())
			{
				_escapeAction.target.z = _unit->getPosition().z;
			}
		}

		tries++;

		// THINK, DAMN YOU
		tile = _save->getTile(_escapeAction.target);
		int distanceFromTarget = _aggroTarget ? Position::distance2d(_aggroTarget->getPosition(), _escapeAction.target) : 0;
		if (dist >= distanceFromTarget)
		{
			score -= (distanceFromTarget - dist) * 10;
		}
		else
		{
			score += (distanceFromTarget - dist) * 10;
		}
		int spotters = 0;
		if (!tile)
		{
			score = -100001; // no you can't quit the battlefield by running off the map.
		}
		else
		{
			spotters = getSpottingUnits(_escapeAction.target);
			if (std::find(_reachable.begin(), _reachable.end(), _save->getTileIndex(_escapeAction.target))  == _reachable.end())
				continue; // just ignore unreachable tiles

			if (_spottingEnemies || spotters)
			{
				if (_spottingEnemies <= spotters)
				{
					score -= (1 + spotters - _spottingEnemies) * EXPOSURE_PENALTY; // that's for giving away our position
				}
				else
				{
					score += (_spottingEnemies - spotters) * EXPOSURE_PENALTY;
				}
			}
			if (tile->getFire())
			{
				score -= FIRE_PENALTY;
			}
			if (tile->getDangerous())
			{
				score -= BASE_SYSTEMATIC_SUCCESS;
			}

			if (_traceAI)
			{
				tile->setMarkerColor(score < 0 ? 3 : (score < FAST_PASS_THRESHOLD/2 ? 8 : (score < FAST_PASS_THRESHOLD ? 9 : 5)));
				tile->setPreview(10);
				tile->setTUMarker(score);
			}

		}

		if (tile && score > bestTileScore)
		{
			// calculate TUs to tile; we could be getting this from findReachable() somehow but that would break something for sure...
			_save->getPathfinding()->calculate(_unit, _escapeAction.target, _escapeAction.getMoveType());
			if (_escapeAction.target == _unit->getPosition() || _save->getPathfinding()->getStartDirection() != -1)
			{
				bestTileScore = score;
				bestTile = _escapeAction.target;
				run = _escapeAction.run;
				_escapeTUs = _save->getPathfinding()->getTotalTUCost();
				if (_escapeAction.target == _unit->getPosition())
				{
					_escapeTUs = 1;
				}
				if (_traceAI)
				{
					tile->setMarkerColor(score < 0 ? 7 : (score < FAST_PASS_THRESHOLD/2 ? 10 : (score < FAST_PASS_THRESHOLD ? 4 : 5)));
					tile->setPreview(10);
					tile->setTUMarker(score);
				}
			}
			_save->getPathfinding()->abortPath();
			if (bestTileScore > FAST_PASS_THRESHOLD) coverFound = true; // good enough, gogogo
		}
	}
	_escapeAction.target = bestTile;
	_escapeAction.run = run;
	if (_traceAI)
	{
		_save->getTile(_escapeAction.target)->setMarkerColor(13);
	}

	if (bestTileScore <= -100000)
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Escape estimation failed.";
		}
		_escapeAction.type = BA_RETHINK; // do something, just don't look dumbstruck :P
		return;
	}
	else
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Escape estimation completed after " << tries << " tries, " << Position::distance2d(_unit->getPosition(), bestTile) << " squares or so away.";
		}
		_escapeAction.type = BA_WALK;
	}
}

/**
 * Counts how many targets, both xcom and civilian are known to this unit
 * @return how many targets are known to us.
 */
int AIModule::countKnownTargets() const
{
	int knownEnemies = 0;

	if (_unit->getFaction() == FACTION_HOSTILE)
	{
		for (auto* bu : *_save->getUnits())
		{
			if (validTarget(bu, true, true))
			{
				++knownEnemies;
			}
		}
	}
	return knownEnemies;
}

/*
 * counts how many enemies (xcom only) are spotting any given position.
 * @param pos the Position to check for spotters.
 * @return spotters.
 */
int AIModule::getSpottingUnits(const Position& pos) const
{
	// if we don't actually occupy the position being checked, we need to do a virtual LOF check.
	bool checking = pos != _unit->getPosition();
	int tally = 0;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, false, false))
		{
			int dist = Position::distance2d(pos, bu->getPosition());
			if (dist > 20) continue;
			Position originVoxel = _save->getTileEngine()->getSightOriginVoxel(bu);
			originVoxel.z -= 2;
			Position targetVoxel;
			if (checking)
			{
				if (_save->getTileEngine()->canTargetUnit(&originVoxel, _save->getTile(pos), &targetVoxel, bu, false, _unit))
				{
					tally++;
				}
			}
			else
			{
				if (_save->getTileEngine()->canTargetUnit(&originVoxel, _save->getTile(pos), &targetVoxel, bu, false))
				{
					tally++;
				}
			}
		}
	}
	return tally;
}

/**
 * Selects the nearest known living target we can see/reach and returns the number of visible enemies.
 * This function includes civilians as viable targets.
 * @return viable targets.
 */
int AIModule::selectNearestTarget()
{
	int tally = 0;
	_closestDist= 100;
	_aggroTarget = 0;
	Position target;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, _unit->getFaction() == FACTION_HOSTILE) &&
			_save->getTileEngine()->visible(_unit, bu->getTile()))
		{
			tally++;
			int dist = Position::distance2d(_unit->getPosition(), bu->getPosition());
			if (dist < _closestDist)
			{
				bool valid = false;
				if (_rifle || !_melee)
				{
					BattleAction action;
					action.actor = _unit;
					action.weapon = _attackAction.weapon;
					action.target = bu->getPosition();
					Position origin = _save->getTileEngine()->getOriginVoxel(action, 0);
					valid = _save->getTileEngine()->canTargetUnit(&origin, bu->getTile(), &target, _unit, false);
				}
				else
				{
					if (selectPointNearTarget(bu, _unit->getTimeUnits()))
					{
						int dir = _save->getTileEngine()->getDirectionTo(_attackAction.target, bu->getPosition());
						valid = _save->getTileEngine()->validMeleeRange(_attackAction.target, dir, _unit, bu, 0);
					}
				}
				if (valid)
				{
					_closestDist = dist;
					_aggroTarget = bu;
				}
			}
		}
	}
	if (_aggroTarget)
	{
		return tally;
	}

	return 0;
}

/**
 * Selects the nearest known living target we can see/reach and returns the number of visible enemies.
 * This function includes civilians as viable targets.
 * Note: Differs from selectNearestTarget() in calling selectPointNearTargetLeeroy().
 * @return viable targets.
 */
int AIModule::selectNearestTargetLeeroy(bool canRun)
{
	int tally = 0;
	_closestDist = 100;
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, _unit->getFaction() == FACTION_HOSTILE) &&
			_save->getTileEngine()->visible(_unit, bu->getTile()))
		{
			tally++;
			int dist = Position::distance2d(_unit->getPosition(), bu->getPosition());
			if (dist < _closestDist)
			{
				bool valid = false;
				if (selectPointNearTargetLeeroy(bu, canRun))
				{
					int dir = _save->getTileEngine()->getDirectionTo(_attackAction.target, bu->getPosition());
					valid = _save->getTileEngine()->validMeleeRange(_attackAction.target, dir, _unit, bu, 0);
				}
				if (valid)
				{
					_closestDist = dist;
					_aggroTarget = bu;
				}
			}
		}
	}
	if (_aggroTarget)
	{
		return tally;
	}

	return 0;
}

/**
 * Selects the nearest known living Xcom unit.
 * used for ambush calculations
 * @return if we found one.
 */
bool AIModule::selectClosestKnownEnemy()
{
	_aggroTarget = 0;
	int minDist = 255;
	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, false))
		{
			int dist = Position::distance2d(bu->getPosition(), _unit->getPosition());
			if (dist < minDist)
			{
				minDist = dist;
				_aggroTarget = bu;
			}
		}
	}
	return _aggroTarget != 0;
}

/**
 * Selects a random known living Xcom or civilian unit.
 * @return if we found one.
 */
bool AIModule::selectRandomTarget()
{
	int farthest = -100;
	_aggroTarget = 0;

	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, _unit->getFaction() == FACTION_HOSTILE))
		{
			int dist = RNG::generate(0,20) - Position::distance2d(_unit->getPosition(), bu->getPosition());
			if (dist > farthest)
			{
				farthest = dist;
				_aggroTarget = bu;
			}
		}
	}
	return _aggroTarget != 0;
}

/**
 * Selects a point near enough to our target to perform a melee attack.
 * @param target Pointer to a target.
 * @param maxTUs Maximum time units the path to the target can cost.
 * @return True if a point was found.
 */
bool AIModule::selectPointNearTarget(BattleUnit *target, int maxTUs)
{
	int size = _unit->getArmor()->getSize();
	int sizeTarget = target->getArmor()->getSize();
	int dirTarget = target->getDirection();
	float dodgeChanceDiff = target->getArmor()->getMeleeDodge(target) * target->getArmor()->getMeleeDodgeBackPenalty() * _attackAction.diff / 160.0f;
	bool returnValue = false;
	int distance = 1000;
	for (int z = -1; z <= 1; ++z)
	{
		for (int x = -size; x <= sizeTarget; ++x)
		{
			for (int y = -size; y <= sizeTarget; ++y)
			{
				if (x || y) // skip the unit itself
				{
					Position checkPath = target->getPosition() + Position (x, y, z);
					if (_save->getTile(checkPath) == 0 || std::find(_reachable.begin(), _reachable.end(), _save->getTileIndex(checkPath))  == _reachable.end())
						continue;
					int dir = _save->getTileEngine()->getDirectionTo(checkPath, target->getPosition());
					bool valid = _save->getTileEngine()->validMeleeRange(checkPath, dir, _unit, target, 0);
					bool fitHere = _save->setUnitPosition(_unit, checkPath, true);

					if (valid && fitHere && !_save->getTile(checkPath)->getDangerous())
					{
						_save->getPathfinding()->calculate(_unit, checkPath, BAM_NORMAL, 0, maxTUs);

						//for 100% dodge diff and on 4th difficulty it will allow aliens to move 10 squares around to made attack from behind.
						int distanceCurrent = _save->getPathfinding()->getPath().size() - dodgeChanceDiff * _save->getTileEngine()->getArcDirection(dir - 4, dirTarget);
						if (_save->getPathfinding()->getStartDirection() != -1 && distanceCurrent < distance)
						{
							_attackAction.target = checkPath;
							returnValue = true;
							distance = distanceCurrent;
						}
						_save->getPathfinding()->abortPath();
					}
				}
			}
		}
	}
	return returnValue;
}

/**
 * Selects a point near enough to our target to perform a melee attack.
 * Note: Differs from selectPointNearTarget() in that it doesn't consider:
 *  - remaining TUs (charge even if not enough TUs to attack)
 *  - dangerous tiles (grenades? pfff!)
 *  - melee dodge (not intelligent enough to attack from behind)
 * @param target Pointer to a target.
 * @return True if a point was found.
 */
bool AIModule::selectPointNearTargetLeeroy(BattleUnit *target, bool canRun)
{
	int size = _unit->getArmor()->getSize();
	int targetsize = target->getArmor()->getSize();
	bool returnValue = false;
	unsigned int distance = 1000;
	for (int z = -1; z <= 1; ++z)
	{
		for (int x = -size; x <= targetsize; ++x)
		{
			for (int y = -size; y <= targetsize; ++y)
			{
				if (x || y) // skip the unit itself
				{
					Position checkPath = target->getPosition() + Position(x, y, z);
					if (_save->getTile(checkPath) == 0)
						continue;
					int dir = _save->getTileEngine()->getDirectionTo(checkPath, target->getPosition());
					bool valid = _save->getTileEngine()->validMeleeRange(checkPath, dir, _unit, target, 0);
					bool fitHere = _save->setUnitPosition(_unit, checkPath, true);

					if (valid && fitHere)
					{
						_save->getPathfinding()->calculate(_unit, checkPath, canRun ? BAM_RUN : BAM_NORMAL, 0, 100000); // disregard unit's TUs.
						if (_save->getPathfinding()->getStartDirection() != -1 && _save->getPathfinding()->getPath().size() < distance)
						{
							_attackAction.target = checkPath;
							returnValue = true;
							distance = _save->getPathfinding()->getPath().size();
						}
						_save->getPathfinding()->abortPath();
					}
				}
			}
		}
	}
	return returnValue;
}

/**
 * Selects a target from a list of units seen by spotter units for out-of-LOS actions and populates _attackAction with the relevant data
 * @return True if we have a target selected
 */
bool AIModule::selectSpottedUnitForSniper()
{
	_aggroTarget = 0;

	// Create a list of spotted targets and the type of attack we'd like to use on each
	std::vector<std::pair<BattleUnit*, BattleAction>> spottedTargets;

	// Get the TU costs for each available attack type
	BattleActionCost costAuto(BA_AUTOSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costSnap(BA_SNAPSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costAimed(BA_AIMEDSHOT, _attackAction.actor, _attackAction.weapon);

	BattleActionCost costThrow;
	// Only want to check throwing if we have a grenade, the default constructor (line above) conveniently returns false from haveTU()
	if (_grenade)
	{
		// We know we have a grenade, now we need to know if we have the TUs to throw it
		costThrow.type = BA_THROW;
		costThrow.actor = _attackAction.actor;
		costThrow.weapon = _unit->getGrenadeFromBelt();
		costThrow.updateTU();
		if (!costThrow.weapon->isFuseEnabled())
		{
			costThrow.Time += 4; // Vanilla TUs for AI picking up grenade from belt
			costThrow += _attackAction.actor->getActionTUs(BA_PRIME, costThrow.weapon);
		}
	}

	for (auto* bu : *_save->getUnits())
	{
		if (validTarget(bu, true, _unit->getFaction() == FACTION_HOSTILE) && bu->getTurnsLeftSpottedForSnipers())
		{
			// Determine which firing mode to use based on how many hits we expect per turn and the unit's intelligence/aggression
			_aggroTarget = bu;
			_attackAction.type = BA_RETHINK;
			_attackAction.target = bu->getPosition();
			extendedFireModeChoice(costAuto, costSnap, costAimed, costThrow, true);

			BattleAction chosenAction = _attackAction;
			if (chosenAction.type == BA_THROW)
				chosenAction.weapon = costThrow.weapon;

			if (_attackAction.type != BA_RETHINK)
			{
				std::pair<BattleUnit*, BattleAction> spottedTarget;
				spottedTarget = std::make_pair(bu, chosenAction);
				spottedTargets.push_back(spottedTarget);
			}
		}
	}

	int numberOfTargets = static_cast<int>(spottedTargets.size());

	if (numberOfTargets) // Now that we have a list of valid targets, pick one and return.
	{
		int pick = RNG::generate(0, numberOfTargets - 1);
		_aggroTarget = spottedTargets.at(pick).first;
		_attackAction.target = _aggroTarget->getPosition();
		_attackAction.type = spottedTargets.at(pick).second.type;
		_attackAction.weapon = spottedTargets.at(pick).second.weapon;
	}
	else // We didn't find a suitable target
	{
		// Make sure we reset anything we might have changed while testing for targets
		_aggroTarget = 0;
		_attackAction.type = BA_RETHINK;
		_attackAction.weapon = _unit->getMainHandWeapon(false);
	}

	return _aggroTarget != 0;
}

/**
 * Scores a firing mode for a particular target based on a accuracy / TUs ratio
 * @param action Pointer to the BattleAction determining the firing mode
 * @param target Pointer to the BattleUnit we're trying to target
 * @param checkLOF Set to true if you want to check for a valid line of fire
 * @return The calculated score
 */
int AIModule::scoreFiringMode(BattleAction *action, BattleUnit *target, bool checkLOF)
{
	// Sanity check first, if the passed action has no type or weapon, return 0.
	if (!action->type || !action->weapon)
	{
		return 0;
	}

	// Get base accuracy for the action
	int accuracy = BattleUnit::getFiringAccuracy(BattleActionAttack::GetBeforeShoot(*action), _save->getBattleGame()->getMod());
	int distanceSq = _unit->distance3dToUnitSq(target);
	int distance = (int)std::ceil(sqrt(float(distanceSq)));

	if (Options::battleUFOExtenderAccuracy && action->type != BA_THROW)
	{
		int upperLimit;
		if (action->type == BA_AIMEDSHOT)
		{
			upperLimit = action->weapon->getRules()->getAimRange();
		}
		else if (action->type == BA_AUTOSHOT)
		{
			upperLimit = action->weapon->getRules()->getAutoRange();
		}
		else
		{
			upperLimit = action->weapon->getRules()->getSnapRange();
		}
		int lowerLimit = action->weapon->getRules()->getMinRange();

		if (distance > upperLimit)
		{
			accuracy -= (distance - upperLimit) * action->weapon->getRules()->getDropoff();
		}
		else if (distance < lowerLimit)
		{
			accuracy -= (lowerLimit - distance) * action->weapon->getRules()->getDropoff();
		}
	}

	if (action->type != BA_THROW && action->weapon->getRules()->isOutOfRange(distanceSq))
		accuracy = 0;

	int numberOfShots = 1;
	if (action->type == BA_AIMEDSHOT)
	{
		numberOfShots = action->weapon->getRules()->getConfigAimed()->shots;
	}
	else if (action->type == BA_SNAPSHOT)
	{
		numberOfShots = action->weapon->getRules()->getConfigSnap()->shots;
	}
	else if (action->type == BA_AUTOSHOT)
	{
		numberOfShots = action->weapon->getRules()->getConfigAuto()->shots;
	}

	int tuCost = _unit->getActionTUs(action->type, action->weapon).Time;
	// Need to include TU cost of getting grenade from belt + priming if we're checking throwing
	if (action->type == BA_THROW && _grenade)
	{
		tuCost = _unit->getActionTUs(action->type, _unit->getGrenadeFromBelt()).Time;
		tuCost += 4;
		tuCost += _unit->getActionTUs(BA_PRIME, _unit->getGrenadeFromBelt()).Time;
	}
	int tuTotal = _unit->getBaseStats()->tu;

	// Return a score of zero if this firing mode doesn't exist for this weapon
	if (!tuCost)
	{
		return 0;
	}

	if (checkLOF)
	{
		Position origin = _save->getTileEngine()->getOriginVoxel((*action), 0);
		Position targetPosition;

		if (action->weapon->getArcingShot(action->type) || action->type == BA_THROW)
		{
			targetPosition = target->getPosition().toVoxel() + Position (8,8, (1 + -target->getTile()->getTerrainLevel()));
			if (!_save->getTileEngine()->validateThrow((*action), origin, targetPosition, _save->getDepth()))
			{
				return 0;
			}
		}
		else
		{
			if (!_save->getTileEngine()->canTargetUnit(&origin, target->getTile(), &targetPosition, _unit, false, target))
			{
				return 0;
			}
		}
	}

	return accuracy * numberOfShots * tuTotal / tuCost;
}

/**
 * Selects an AI mode based on a number of factors, some RNG and the results of the rest of the determinations.
 */
void AIModule::evaluateAIMode()
{
	if ((_unit->getCharging() && _attackAction.type != BA_RETHINK))
	{
		_AIMode = AI_COMBAT;
		return;
	}
	// don't try to run away as often if we're a melee type, and really don't try to run away if we have a viable melee target, or we still have 50% or more TUs remaining.
	int escapeOdds = 15;
	if (_melee)
	{
		escapeOdds = 12;
	}
	if (_unit->getFaction() == FACTION_HOSTILE && (_unit->getTimeUnits() > _unit->getBaseStats()->tu / 2 || _unit->getCharging()))
	{
		escapeOdds = 5;
	}
	int ambushOdds = 12;
	int combatOdds = 20;
	// we're less likely to patrol if we see enemies.
	int patrolOdds = _visibleEnemies ? 15 : 30;

	// the enemy sees us, we should take retreat into consideration, and forget about patrolling for now.
	if (_spottingEnemies)
	{
		patrolOdds = 0;
		if (_escapeTUs == 0)
		{
			setupEscape();
		}
	}

	// melee/blaster units shouldn't consider ambush
	if (!_rifle || _ambushTUs == 0)
	{
		ambushOdds = 0;
		if (_melee)
		{
			combatOdds *= 1.3;
		}
	}

	// if we KNOW there are enemies around...
	if (_knownEnemies)
	{
		if (_knownEnemies == 1)
		{
			combatOdds *= 1.2;
		}

		if (_escapeTUs == 0)
		{
			if (selectClosestKnownEnemy())
			{
				setupEscape();
			}
			else
			{
				escapeOdds = 0;
			}
		}
	}
	else if (_unit->getFaction() == FACTION_HOSTILE)
	{
		combatOdds = 0;
		escapeOdds = 0;
	}

	// take our current mode into consideration
	switch (_AIMode)
	{
	case AI_PATROL:
		patrolOdds *= 1.1;
		break;
	case AI_AMBUSH:
		ambushOdds *= 1.1;
		break;
	case AI_COMBAT:
		combatOdds *= 1.1;
		break;
	case AI_ESCAPE:
		escapeOdds *= 1.1;
		break;
	}

	// take our overall health into consideration
	if (_unit->getHealth() < _unit->getBaseStats()->health / 3)
	{
		escapeOdds *= 1.7;
		combatOdds *= 0.6;
		ambushOdds *= 0.75;
	}
	else if (_unit->getHealth() < 2 * (_unit->getBaseStats()->health / 3))
	{
		escapeOdds *= 1.4;
		combatOdds *= 0.8;
		ambushOdds *= 0.8;
	}
	else if (_unit->getHealth() < _unit->getBaseStats()->health)
	{
		escapeOdds *= 1.1;
	}

	// take our aggression into consideration
	switch (_unit->getAggression())
	{
	case 0:
		escapeOdds *= 1.4;
		combatOdds *= 0.7;
		break;
	case 1:
		ambushOdds *= 1.1;
		break;
	case 2:
		combatOdds *= 1.4;
		escapeOdds *= 0.7;
		break;
	default:
		combatOdds *= Clamp(1.2 + (_unit->getAggression() / 10.0), 0.1, 2.0);
		escapeOdds *= Clamp(0.9 - (_unit->getAggression() / 10.0), 0.1, 2.0);
		break;
	}

	if (_AIMode == AI_COMBAT)
	{
		ambushOdds *= 1.5;
	}

	// factor in the spotters.
	if (_spottingEnemies)
	{
		escapeOdds = 10 * escapeOdds * (_spottingEnemies + 10) / 100;
		combatOdds = 5 * combatOdds * (_spottingEnemies + 20) / 100;
	}
	else
	{
		escapeOdds /= 2;
	}

	// factor in visible enemies.
	if (_visibleEnemies)
	{
		combatOdds = 10 * combatOdds * (_visibleEnemies + 10) /100;
		if (_closestDist < 5)
		{
			ambushOdds = 0;
		}
	}
	// make sure we have an ambush lined up, or don't even consider it.
	if (_ambushTUs)
	{
		ambushOdds *= 1.7;
	}
	else
	{
		ambushOdds = 0;
	}

	// factor in mission type
	if (_save->getMissionType() == "STR_BASE_DEFENSE")
	{
		escapeOdds *= 0.75;
		ambushOdds *= 0.6;
	}

	// no weapons, not psychic? don't pick combat or ambush
	if (!_melee && !_rifle && !_blaster && !_grenade && _unit->getBaseStats()->psiSkill == 0)
	{
		combatOdds = 0;
		ambushOdds = 0;
	}
	// generate a random number to represent our decision.
	int decision = RNG::generate(1, std::max(1, patrolOdds + ambushOdds + escapeOdds + combatOdds));

	if (decision > escapeOdds)
	{
		if (decision > escapeOdds + ambushOdds)
		{
			if (decision > escapeOdds + ambushOdds + combatOdds)
			{
				_AIMode = AI_PATROL;
			}
			else
			{
				_AIMode = AI_COMBAT;
			}
		}
		else
		{
			_AIMode = AI_AMBUSH;
		}
	}
	else
	{
		_AIMode = AI_ESCAPE;
	}

	// if the aliens are cheating, or the unit is charging, enforce combat as a priority.
	if ((_unit->getFaction() == FACTION_HOSTILE && _save->isCheating()) || _unit->getCharging() != 0)
	{
		_AIMode = AI_COMBAT;
	}


	// enforce the validity of our decision, and try fallback behaviour according to priority.
	if (_AIMode == AI_COMBAT)
	{
		if (_save->getTile(_attackAction.target) && _save->getTile(_attackAction.target)->getUnit())
		{
			if (_attackAction.type != BA_RETHINK)
			{
				return;
			}
			if (findFirePoint())
			{
				return;
			}
		}
		else if (selectRandomTarget() && findFirePoint())
		{
			return;
		}
		_AIMode = AI_PATROL;
	}

	if (_AIMode == AI_PATROL)
	{
		if (_toNode || _foundBaseModuleToDestroy)
		{
			return;
		}
		_AIMode = AI_AMBUSH;
	}

	if (_AIMode == AI_AMBUSH)
	{
		if (_ambushTUs != 0)
		{
			return;
		}
		_AIMode = AI_ESCAPE;
	}
}

/**
 * Find a position where we can see our target, and move there.
 * check the 11x11 grid for a position nearby where we can potentially target him.
 * @return True if a possible position was found.
 */
bool AIModule::findFirePoint()
{
	if (!selectClosestKnownEnemy())
		return false;
	std::vector<Position> randomTileSearch = _save->getTileSearch(); // copy!
	RNG::shuffle(randomTileSearch);
	Position target;
	const int BASE_SYSTEMATIC_SUCCESS = 100;
	const int FAST_PASS_THRESHOLD = 125;
	bool waitIfOutsideWeaponRange = _unit->getGeoscapeSoldier() ? false : _unit->getUnitRules()->waitIfOutsideWeaponRange();
	bool extendedFireModeChoiceEnabled = _save->getBattleGame()->getMod()->getAIExtendedFireModeChoice();
	int bestScore = 0;
	_attackAction.type = BA_RETHINK;
	for (const auto& randomPosition : randomTileSearch)
	{
		Position pos = _unit->getPosition() + randomPosition;
		Tile *tile = _save->getTile(pos);
		if (tile == 0  ||
			std::find(_reachableWithAttack.begin(), _reachableWithAttack.end(), _save->getTileIndex(pos))  == _reachableWithAttack.end())
			continue;
		int score = 0;
		// i should really make a function for this
		Position origin = pos.toVoxel() +
			// 4 because -2 is eyes and 2 below that is the rifle (or at least that's my understanding)
			Position(8,8, _unit->getHeight() + _unit->getFloatHeight() - tile->getTerrainLevel() - 4);

		if (_save->getTileEngine()->canTargetUnit(&origin, _aggroTarget->getTile(), &target, _unit, false))
		{
			_save->getPathfinding()->calculate(_unit, pos, BAM_NORMAL);
			// can move here
			if (_save->getPathfinding()->getStartDirection() != -1)
			{
				score = BASE_SYSTEMATIC_SUCCESS - getSpottingUnits(pos) * 10;
				score += _unit->getTimeUnits() - _save->getPathfinding()->getTotalTUCost();
				if (!_aggroTarget->checkViewSector(pos))
				{
					score += 10;
				}

				// Extended behavior: if we have a limited-range weapon, bump up the score for getting closer to the target, down for further
				if (!waitIfOutsideWeaponRange && extendedFireModeChoiceEnabled)
				{
					int distanceToTargetSq = _unit->distance3dToUnitSq(_aggroTarget);
					int distanceToTarget = (int)std::ceil(sqrt(float(distanceToTargetSq)));
					if (_attackAction.weapon && _attackAction.weapon->getRules()->isOutOfRange(distanceToTargetSq)) // make sure we can get the ruleset before checking the range
					{
						int proposedDistance = Position::distance2d(pos, _aggroTarget->getPosition());
						proposedDistance = std::max(proposedDistance, 1);
						score = score * distanceToTarget / proposedDistance;
					}
				}

				if (score > bestScore)
				{
					bestScore = score;
					_attackAction.target = pos;
					_attackAction.finalFacing = _save->getTileEngine()->getDirectionTo(pos, _aggroTarget->getPosition());
					if (score > FAST_PASS_THRESHOLD)
					{
						break;
					}
				}
			}
		}
	}

	if (bestScore > 70)
	{
		_attackAction.type = BA_WALK;
		if (_traceAI)
		{
			Log(LOG_INFO) << "Firepoint found at " << _attackAction.target << ", with a score of: " << bestScore;
		}
		return true;
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Firepoint failed, best estimation was: " << _attackAction.target << ", with a score of: " << bestScore;
	}

	return false;
}

/**
 * Decides if it worth our while to create an explosion here.
 * @param targetPos The target's position.
 * @param attackingUnit The attacking unit.
 * @param radius How big the explosion will be.
 * @param diff Game difficulty.
 * @param grenade Is the explosion coming from a grenade?
 * @return Value greater than zero if it is worthwhile creating an explosion in the target position. Bigger value better target.
 */
int AIModule::explosiveEfficacy(Position targetPos, BattleUnit *attackingUnit, int radius, int diff, bool grenade) const
{
	Tile *targetTile = _save->getTile(targetPos);

	// don't throw grenades at flying enemies.
	if (grenade && targetPos.z > 0 && targetTile->hasNoFloor(_save))
	{
		return false;
	}

	if (diff == -1)
	{
		diff = _save->getBattleState()->getGame()->getSavedGame()->getDifficultyCoefficient();
	}
	int distance = Position::distance2d(attackingUnit->getPosition(), targetPos);
	int injurylevel = attackingUnit->getBaseStats()->health - attackingUnit->getHealth();
	int desperation = (100 - attackingUnit->getMorale()) / 10;
	int enemiesAffected = 0;
	// if we're below 1/3 health, let's assume things are dire, and increase desperation.
	if (injurylevel > (attackingUnit->getBaseStats()->health / 3) * 2)
		desperation += 3;

	int efficacy = desperation;

	// don't go kamikaze unless we're already doomed.
	if (abs(attackingUnit->getPosition().z - targetPos.z) <= Options::battleExplosionHeight && distance <= radius)
	{
		efficacy -= 4;
	}

	// allow difficulty to have its influence
	efficacy += diff/2;

	// account for the unit we're targetting
	BattleUnit *target = targetTile->getUnit();
	if (target && !targetTile->getDangerous())
	{
		++enemiesAffected;
		++efficacy;
	}

	for (auto* bu : *_save->getUnits())
	{
			// don't grenade dead guys
		if (!bu->isOut() &&
			// don't count ourself twice
			bu != attackingUnit &&
			// don't count the target twice
			bu != target &&
			// don't count units that probably won't be affected cause they're out of range
			abs(bu->getPosition().z - targetPos.z) <= Options::battleExplosionHeight &&
			Position::distance2d(bu->getPosition(), targetPos) <= radius)
		{
				// don't count people who were already grenaded this turn
			if (bu->getTile()->getDangerous() ||
				// don't count units we don't know about
				(bu->getFaction() == _targetFaction && bu->getTurnsSinceSpotted() > _intelligence))
				continue;

			// trace a line from the grenade origin to the unit we're checking against
			Position voxelPosA = Position (targetPos.toVoxel() + TileEngine::voxelTileCenter);
			Position voxelPosB = Position (bu->getPosition().toVoxel() + TileEngine::voxelTileCenter);
			std::vector<Position> traj;
			int collidesWith = _save->getTileEngine()->calculateLineVoxel(voxelPosA, voxelPosB, false, &traj, target, bu);

			if (collidesWith == V_UNIT && traj.front().toTile() == bu->getPosition())
			{
				if (bu->getFaction() == _targetFaction)
				{
					++enemiesAffected;
					++efficacy;
				}
				else if (bu->getFaction() == attackingUnit->getFaction() || (attackingUnit->getFaction() == FACTION_NEUTRAL && bu->getFaction() == FACTION_PLAYER))
					efficacy -= 2; // friendlies count double
			}
		}
	}
	// don't throw grenades at single targets, unless morale is in the danger zone
	// or we're halfway towards panicking while bleeding to death.
	if (grenade && desperation < 6 && enemiesAffected < 2)
	{
		return 0;
	}

	if (enemiesAffected >= 10)
	{
		// Ignore loses if we can kill lot of enemies.
		return enemiesAffected;
	}
	else if (efficacy > 0)
	{
		// We kill more enemies than allies.
		return efficacy;
	}
	else
	{
		return 0;
	}
}

/**
 * Attempts to take a melee attack/charge an enemy we can see.
 * Melee targetting: we can see an enemy, we can move to it so we're charging blindly toward an enemy.
 */
void AIModule::meleeAction()
{
	BattleActionCost attackCost(BA_HIT, _unit, _unit->getUtilityWeapon(BT_MELEE));
	if (!attackCost.haveTU())
	{
		// cannot make a melee attack - consider some other behaviour, like running away, or standing motionless.
		return;
	}
	if (_aggroTarget != 0 && !_aggroTarget->isOut())
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
			return;
		}
	}
	int chargeReserve = std::min(_unit->getTimeUnits() - attackCost.Time, 2 * (_unit->getEnergy() - attackCost.Energy));
	int distance = (chargeReserve / 4) + 1;
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		int newDistance = Position::distance2d(_unit->getPosition(), bu->getPosition());
		if (newDistance > 20 ||
			!validTarget(bu, true, _unit->getFaction() == FACTION_HOSTILE))
			continue;
		//pick closest living unit that we can move to
		if ((newDistance < distance || newDistance == 1) && !bu->isOut())
		{
			if (newDistance == 1 || selectPointNearTarget(bu, chargeReserve))
			{
				_aggroTarget = bu;
				_attackAction.type = BA_WALK;
				_unit->setCharging(_aggroTarget);
				distance = newDistance;
			}

		}
	}
	if (_aggroTarget != 0)
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
		}
	}
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "AIModule::meleeAction:" << " [target]: " << (_aggroTarget->getId()) << " at: "  << _attackAction.target; }
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "CHARGE!"; }
}

/**
 * Attempts to take a melee attack/charge an enemy we can see.
 * Melee targetting: we can see an enemy, we can move to it so we're charging blindly toward an enemy.
 * Note: Differs from meleeAction() in calling selectPointNearTargetLeeroy() and ignoring some more checks.
 */
void AIModule::meleeActionLeeroy(bool canRun)
{
	if (_aggroTarget != 0 && !_aggroTarget->isOut())
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
			return;
		}
	}
	int distance = 1000;
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		int newDistance = Position::distance2d(_unit->getPosition(), bu->getPosition());
		if (!validTarget(bu, true, _unit->getFaction() == FACTION_HOSTILE))
			continue;
		//pick closest living unit
		if ((newDistance < distance || newDistance == 1) && !bu->isOut())
		{
			if (newDistance == 1 || selectPointNearTargetLeeroy(bu, canRun))
			{
				_aggroTarget = bu;
				_attackAction.type = BA_WALK;
				_attackAction.run = canRun;
				_unit->setCharging(_aggroTarget);
				distance = newDistance;
			}

		}
	}
	if (_aggroTarget != 0)
	{
		if (_save->getTileEngine()->validMeleeRange(_unit, _aggroTarget, _save->getTileEngine()->getDirectionTo(_unit->getPosition(), _aggroTarget->getPosition())))
		{
			meleeAttack();
		}
	}
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "AIModule::meleeAction:" << " [target]: " << (_aggroTarget->getId()) << " at: " << _attackAction.target; }
	if (_traceAI && _aggroTarget) { Log(LOG_INFO) << "CHARGE!"; }
}

/**
 * Attempts to fire a waypoint projectile at an enemy we, or one of our teammates sees.
 *
 * Waypoint targeting: pick from any units currently spotted by our allies.
 */
void AIModule::wayPointAction()
{
	BattleActionCost attackCost(BA_LAUNCH, _unit, _attackAction.weapon);
	if (!attackCost.haveTU())
	{
		// cannot make a launcher attack - consider some other behaviour, like running away, or standing motionless.
		return;
	}
	_aggroTarget = 0;
	for (auto* bu : *_save->getUnits())
	{
		if (_aggroTarget != 0) break; // loop finished
		if (!validTarget(bu, true, _unit->getFaction() == FACTION_HOSTILE))
		{
			continue;
		}
		_save->getPathfinding()->calculate(_unit, bu->getPosition(), BAM_MISSILE, bu, -1);
		BattleItem* ammo = _attackAction.weapon->getAmmoForAction(BA_LAUNCH);
		if (_save->getPathfinding()->getStartDirection() != -1 &&
			explosiveEfficacy(bu->getPosition(), _unit, ammo->getRules()->getExplosionRadius({ BA_LAUNCH, _unit, _attackAction.weapon, ammo }), _attackAction.diff))
		{
			_aggroTarget = bu;
		}
		_save->getPathfinding()->abortPath();
	}

	if (_aggroTarget != 0)
	{
		_attackAction.type = BA_LAUNCH;
		_attackAction.updateTU();
		if (!_attackAction.haveTU())
		{
			_attackAction.type = BA_RETHINK;
			return;
		}
		_attackAction.waypoints.clear();

		int PathDirection;
		int CollidesWith;
		int maxWaypoints = _attackAction.weapon->getCurrentWaypoints();
		if (maxWaypoints == -1)
		{
			maxWaypoints = 6 + (_attackAction.diff * 2);
		}
		Position LastWayPoint = _unit->getPosition();
		Position LastPosition = _unit->getPosition();
		Position CurrentPosition = _unit->getPosition();
		Position DirectionVector;

		_save->getPathfinding()->calculate(_unit, _aggroTarget->getPosition(), BAM_MISSILE, _aggroTarget, -1);
		PathDirection = _save->getPathfinding()->dequeuePath();
		while (PathDirection != -1 && (int)_attackAction.waypoints.size() < maxWaypoints)
		{
			LastPosition = CurrentPosition;
			_save->getPathfinding()->directionToVector(PathDirection, &DirectionVector);
			CurrentPosition = CurrentPosition + DirectionVector;
			Position voxelPosA ((CurrentPosition.x * 16)+8, (CurrentPosition.y * 16)+8, (CurrentPosition.z * 24)+16);
			Position voxelPosb ((LastWayPoint.x * 16)+8, (LastWayPoint.y * 16)+8, (LastWayPoint.z * 24)+16);
			CollidesWith = _save->getTileEngine()->calculateLineVoxel(voxelPosA, voxelPosb, false, nullptr, _unit);
			if (CollidesWith > V_EMPTY && CollidesWith < V_UNIT)
			{
				_attackAction.waypoints.push_back(LastPosition);
				LastWayPoint = LastPosition;
			}
			else if (CollidesWith == V_UNIT)
			{
				BattleUnit* target = _save->getTile(CurrentPosition)->getOverlappingUnit(_save);
				if (target == _aggroTarget)
				{
					_attackAction.waypoints.push_back(CurrentPosition);
					LastWayPoint = CurrentPosition;
				}
			}
			PathDirection = _save->getPathfinding()->dequeuePath();
		}
		_attackAction.target = _attackAction.waypoints.front();
		if (LastWayPoint != _aggroTarget->getPosition())
		{
			_attackAction.type = BA_RETHINK;
		}
	}
}

/**
 * Attempts to fire at an enemy spotted for us.
 *
 */
bool AIModule::sniperAction()
{
	if (_traceAI) { Log(LOG_INFO) << "Attempting sniper action..."; }

	if (selectSpottedUnitForSniper())
	{
		_visibleEnemies = std::max(_visibleEnemies, 1); // Make sure we count at least our target as visible, otherwise we might not shoot!

		if (_traceAI) { Log(LOG_INFO) << "Target for sniper found at (" << _attackAction.target.x << "," << _attackAction.target.y << "," << _attackAction.target.z << ")."; }
		return true;
	}

	if (_traceAI) { Log(LOG_INFO) << "No valid target found or not enough TUs for sniper action."; }
	return false;
}

/**
 * Attempts to fire at an enemy we can see.
 *
 * Regular targeting: we can see an enemy, we have a gun, let's try to shoot.
 */
void AIModule::projectileAction()
{
	_attackAction.target = _aggroTarget->getPosition();
	auto testEffect = [&](BattleActionCost& cost)
	{
		if (cost.haveTU())
		{
			auto attack = BattleActionAttack::GetBeforeShoot(cost);
			if (attack.damage_item == nullptr)
			{
				cost.clearTU();
			}
			else
			{
				int radius = attack.damage_item->getRules()->getExplosionRadius(attack);
				if (radius != 0 && explosiveEfficacy(_attackAction.target, _unit, radius, _attackAction.diff) == 0)
				{
					cost.clearTU();
				}
			}
		}
	};

	int distance = Position::distance2d(_unit->getPosition(), _attackAction.target);
	_attackAction.type = BA_RETHINK;

	BattleActionCost costAuto(BA_AUTOSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costSnap(BA_SNAPSHOT, _attackAction.actor, _attackAction.weapon);
	BattleActionCost costAimed(BA_AIMEDSHOT, _attackAction.actor, _attackAction.weapon);

	testEffect(costAuto);
	testEffect(costSnap);
	testEffect(costAimed);

	// Is the unit willingly waiting outside of weapon's range (e.g. ninja camouflaged in ambush)?
	bool waitIfOutsideWeaponRange = _unit->getGeoscapeSoldier() ? false : _unit->getUnitRules()->waitIfOutsideWeaponRange();

	// Do we want to use the extended firing mode scoring?
	bool extendedFireModeChoiceEnabled = _save->getBattleGame()->getMod()->getAIExtendedFireModeChoice();
	if (!waitIfOutsideWeaponRange && extendedFireModeChoiceEnabled)
	{
		// Note: this will also check for the weapon's max range
		BattleActionCost costThrow; // Not actually checked here, just passed to extendedFireModeChoice as a necessary argument
		extendedFireModeChoice(costAuto, costSnap, costAimed, costThrow, false);
		return;
	}

	// Do we want to check if the weapon is in range?
	bool aiRespectsMaxRange = _save->getBattleGame()->getMod()->getAIRespectMaxRange();
	if (!waitIfOutsideWeaponRange && aiRespectsMaxRange)
	{
		// If we want to check and it's not in range, perhaps we should re-think shooting
		int distanceSq = _unit->distance3dToPositionSq(_attackAction.target);
		if (_attackAction.weapon->getRules()->isOutOfRange(distanceSq))
		{
			return;
		}
	}

	// vanilla
	if (distance < 4)
	{
		if (costAuto.haveTU())
		{
			_attackAction.type = BA_AUTOSHOT;
			return;
		}
		if (!costSnap.haveTU())
		{
			if (costAimed.haveTU())
			{
				_attackAction.type = BA_AIMEDSHOT;
			}
			return;
		}
		_attackAction.type = BA_SNAPSHOT;
		return;
	}


	if (distance > 12)
	{
		if (costAimed.haveTU())
		{
			_attackAction.type = BA_AIMEDSHOT;
			return;
		}
		if (distance < 20 && costSnap.haveTU())
		{
			_attackAction.type = BA_SNAPSHOT;
			return;
		}
	}

	if (costSnap.haveTU())
	{
		_attackAction.type = BA_SNAPSHOT;
		return;
	}
	if (costAimed.haveTU())
	{
		_attackAction.type = BA_AIMEDSHOT;
		return;
	}
	if (costAuto.haveTU())
	{
		_attackAction.type = BA_AUTOSHOT;
	}
}

void AIModule::extendedFireModeChoice(BattleActionCost& costAuto, BattleActionCost& costSnap, BattleActionCost& costAimed, BattleActionCost& costThrow, bool checkLOF)
{
	std::vector<BattleActionType> attackOptions = { };
	if (costAimed.haveTU())
	{
		attackOptions.push_back(BA_AIMEDSHOT);
	}
	if (costAuto.haveTU())
	{
		attackOptions.push_back(BA_AUTOSHOT);
	}
	if (costSnap.haveTU())
	{
		attackOptions.push_back(BA_SNAPSHOT);
	}
	if (costThrow.haveTU())
	{
		attackOptions.push_back(BA_THROW);
	}

	BattleActionType chosenAction = BA_RETHINK;
	BattleAction testAction = _attackAction;
	int score = 0;
	for (auto &i : attackOptions)
	{
		testAction.type = i;
		if (i == BA_THROW)
		{
			if (_grenade)
			{
				testAction.weapon = _unit->getGrenadeFromBelt();
			}
			else
			{
				continue;
			}
		}
		else
		{
			testAction.weapon = _attackAction.weapon;
		}
		int newScore = scoreFiringMode(&testAction, _aggroTarget, checkLOF);

		// Add a random factor to the firing mode score based on intelligence
		// An intelligence value of 10 will decrease this random factor to 0
		// Default values for and intelligence value of 0 will make this a 50% to 150% roll
		int intelligenceModifier = _save->getBattleGame()->getMod()->getAIFireChoiceIntelCoeff() * std::max(10 - _unit->getIntelligence(), 0);
		newScore = newScore * (100 + RNG::generate(-intelligenceModifier, intelligenceModifier)) / 100;

		// More aggressive units get a modifier to the score for auto shots
		// Aggression = 0 lowers the score, aggro = 1 is no modifier, aggro > 1 bumps up the score by 5% (configurable) for each increment over 1
		if (i == BA_AUTOSHOT)
		{
			newScore = newScore * (100 + (_unit->getAggression() - 1) * _save->getBattleGame()->getMod()->getAIFireChoiceAggroCoeff()) / 100;
		}

		if (newScore > score)
		{
			score = newScore;
			chosenAction = i;
		}

		if (_traceAI)
		{
			Log(LOG_INFO) << "Evaluate option " << (int)i << ", score = " << newScore;
		}
	}

	_attackAction.type = chosenAction;
}

/**
 * Evaluates whether to throw a grenade at an enemy (or group of enemies) we can see.
 */
void AIModule::grenadeAction()
{
	// do we have a grenade on our belt?
	BattleItem *grenade = _unit->getGrenadeFromBelt();
	BattleAction action;
	action.weapon = grenade;
	action.type = BA_THROW;
	action.actor = _unit;

	action.updateTU();
	// Xilmi: Take into account that we might already have primed the grenade before
	if (!action.weapon->isFuseEnabled())
	{
		action.Time += 4; // 4TUs for picking up the grenade
		action += _unit->getActionTUs(BA_PRIME, grenade);
	}
	// take into account we might have to turn towards our target
	action.Time += getTurnCostTowards(_aggroTarget->getPosition());
	// do we have enough TUs to prime and throw the grenade?
	if (action.haveTU())
	{
		auto radius = grenade->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(action));
		if (explosiveEfficacy(_aggroTarget->getPosition(), _unit, radius, _attackAction.diff, true))
		{
			action.target = _aggroTarget->getPosition();
		}
		else if (!getNodeOfBestEfficacy(&action, radius))
		{
			return;
		}
		Position originVoxel = _save->getTileEngine()->getOriginVoxel(action, 0);
		Position targetVoxel = action.target.toVoxel() + Position (8,8, (1 + -_save->getTile(action.target)->getTerrainLevel()));
		// are we within range?
		if (_save->getTileEngine()->validateThrow(action, originVoxel, targetVoxel, _save->getDepth()))
		{
			_attackAction.weapon = grenade;
			_attackAction.target = action.target;
			_attackAction.type = BA_THROW;
			_rifle = false;
			_melee = false;
		}
	}
}

/**
 * Attempts a psionic attack on an enemy we "know of".
 *
 * Psionic targetting: pick from any of the "exposed" units.
 * Exposed means they have been previously spotted, and are therefore "known" to the AI,
 * regardless of whether we can see them or not, because we're psychic.
 * @return True if a psionic attack is performed.
 */
bool AIModule::psiAction()
{
	BattleItem *item = _unit->getUtilityWeapon(BT_PSIAMP);
	if (!item)
	{
		return false;
	}

	const int costLength = 3;
	BattleActionCost cost[costLength] =
	{
		BattleActionCost(BA_USE, _unit, item),
		BattleActionCost(BA_PANIC, _unit, item),
		BattleActionCost(BA_MINDCONTROL, _unit, item),
	};
	bool have = false;
	for (int j = 0; j < costLength; ++j)
	{
		if (cost[j].Time > 0)
		{
			cost[j].Time += _escapeTUs;
			cost[j].Energy += _escapeTUs / 2;
			have |= cost[j].haveTU();
		}
	}
	bool LOSRequired = item->getRules()->isLOSRequired();

	_aggroTarget = 0;
		// don't let mind controlled soldiers mind control other soldiers.
	if (_unit->getOriginalFaction() == _unit->getFaction()
		// and we have the required 25 TUs and can still make it to cover
		&& have
		// and we didn't already do a psi action this round
		&& !_didPsi)
	{
		int weightToAttack = 0;
		BattleActionType typeToAttack = BA_NONE;

		for (auto* bu : *_save->getUnits())
		{
			// don't target tanks
			if (bu->getArmor()->getSize() == 1 &&
				validTarget(bu, true, false) &&
				// they must be player units
				bu->getOriginalFaction() == _targetFaction &&
				(!LOSRequired ||
				std::find(_unit->getVisibleUnits()->begin(), _unit->getVisibleUnits()->end(), bu) != _unit->getVisibleUnits()->end()))
			{
				BattleUnit *victim = bu;
				if (item->getRules()->isOutOfRange(_unit->distance3dToUnitSq(victim)))
				{
					continue;
				}
				for (int j = 0; j < costLength; ++j)
				{
					// can't use this attack.
					if (!cost[j].haveTU())
					{
						continue;
					}

					int weightToAttackMe = _save->getTileEngine()->psiAttackCalculate({ cost[j].type, _unit, item, item }, victim);

					// low chance we hit this target.
					if (weightToAttackMe < 0)
					{
						continue;
					}

					// different bonus per attack.
					if (cost[j].type == BA_MINDCONTROL)
					{
						// target cannot be mind controlled
						if (victim->getUnitRules() && !victim->getUnitRules()->canBeMindControlled()) continue;

						int controlOdds = 40;
						int morale = victim->getMorale();
						int bravery = victim->reduceByBravery(10);
						if (bravery > 6)
							controlOdds -= 15;
						if (bravery < 4)
							controlOdds += 15;
						if (morale >= 40)
						{
							if (morale - 10 * bravery < 50)
								controlOdds -= 15;
						}
						else
						{
							controlOdds += 15;
						}
						if (!morale)
						{
							controlOdds = 100;
						}
						if (RNG::percent(controlOdds))
						{
							weightToAttackMe += 60;
						}
						else
						{
							continue;
						}
					}
					else if (cost[j].type == BA_USE)
					{
						if (RNG::percent(80 - _attackAction.diff * 10)) // Star gods have mercy on us.
						{
							continue;
						}
						auto attack = BattleActionAttack{ BA_USE, _unit, item, item };
						int radius = item->getRules()->getExplosionRadius(attack);
						if (radius > 0)
						{
							int efficity = explosiveEfficacy(victim->getPosition(), _unit, radius, _attackAction.diff);
							if (efficity)
							{
								weightToAttackMe += 2 * efficity * _intelligence; //bonus for boom boom.
							}
							else
							{
								continue;
							}
						}
						else
						{
							weightToAttackMe += item->getRules()->getPowerBonus(attack);
						}
					}
					else if (cost[j].type == BA_PANIC)
					{
						// target cannot be panicked
						if (victim->getUnitRules() && !victim->getUnitRules()->canPanic()) continue;

						weightToAttackMe += 40;
					}

					if (weightToAttackMe > weightToAttack)
					{
						typeToAttack = cost[j].type;
						weightToAttack = weightToAttackMe;
						_aggroTarget = victim;
					}
				}
			}
		}

		if (!_aggroTarget || !weightToAttack) return false;

		if (_visibleEnemies && _attackAction.weapon)
		{
			BattleActionType actions[] = {
				BA_AIMEDSHOT,
				BA_AUTOSHOT,
				BA_SNAPSHOT,
				BA_HIT,
			};
			for (auto action : actions)
			{
				auto ammo = _attackAction.weapon->getAmmoForAction(action);
				if (!ammo)
				{
					continue;
				}

				int weightPower = ammo->getRules()->getPowerBonus({ action, _attackAction.actor, _attackAction.weapon, ammo });
				if (action == BA_HIT)
				{
					// prefer psi over melee
					weightPower /= 2;
				}
				else
				{
					// prefer machine guns
					weightPower *= _attackAction.weapon->getActionConf(action)->shots;
				}
				if (weightPower >= weightToAttack)
				{
					return false;
				}
			}
		}
		else if (RNG::generate(35, 155) >= weightToAttack)
		{
			return false;
		}

		if (_traceAI)
		{
			Log(LOG_INFO) << "making a psionic attack this turn";
		}

		_psiAction.type = typeToAttack;
		_psiAction.target = _aggroTarget->getPosition();
		_psiAction.weapon = item;
		return true;
	}
	return false;
}

/**
 * Performs a melee attack action.
 */
void AIModule::meleeAttack()
{
	_unit->lookAt(_aggroTarget->getPosition() + Position(_unit->getArmor()->getSize()-1, _unit->getArmor()->getSize()-1, 0), false);
	while (_unit->getStatus() == STATUS_TURNING)
		_unit->turn();
	if (_traceAI) { Log(LOG_INFO) << "Attack unit: " << _aggroTarget->getId(); }
	_attackAction.target = _aggroTarget->getPosition();
	_attackAction.type = BA_HIT;
	_attackAction.weapon = _unit->getUtilityWeapon(BT_MELEE);
}

/**
 * Validates a target.
 * @param target the target we want to validate.
 * @param assessDanger do we care if this unit was previously targetted with a grenade?
 * @param includeCivs do we include civilians in the threat assessment?
 * @return whether this target is someone we would like to kill.
 */
bool AIModule::validTarget(BattleUnit *target, bool assessDanger, bool includeCivs) const
{
	// ignore units that:
	// 1. are dead/unconscious
	// 2. are dangerous (they have been grenaded)
	// 3. are on our side
	// 4. are hostile/neutral units marked as ignored by the AI
	if (target->isOut() ||
		(assessDanger && target->getTile()->getDangerous()) ||
		(target->getFaction() != FACTION_PLAYER && target->isIgnoredByAI()) ||
		target->getFaction() == _unit->getFaction())
	{
		return false;
	}

	// ignore units that we don't "know" about...
	// ... unless we are a sniper and the spotters know about them
	if (_unit->getFaction() == FACTION_HOSTILE &&
		_intelligence < target->getTurnsSinceSpotted() &&
		(!_unit->isSniper() || !target->getTurnsLeftSpottedForSnipers()))
	{
		return false;
	}

	if (includeCivs)
	{
		return true;
	}

	return target->getFaction() == _targetFaction;
}

/**
 * Checks the alien's reservation setting.
 * @return the reserve setting.
 */
BattleActionType AIModule::getReserveMode()
{
	return _reserve;
}

/**
 * We have a dichotomy on our hands: we have a ranged weapon and melee capability.
 * let's make a determination on which one we'll be using this round.
 */
void AIModule::selectMeleeOrRanged()
{
	BattleItem *range = _attackAction.weapon;
	BattleItem *melee = _unit->getUtilityWeapon(BT_MELEE);

	if (!melee || !melee->haveAnyAmmo())
	{
		// no idea how we got here, but melee is definitely out of the question.
		_melee = false;
		return;
	}
	if (!range || !range->haveAnyAmmo())
	{
		_rifle = false;
		return;
	}

	const RuleItem *meleeRule = melee->getRules();

	int meleeOdds = 10;

	int dmg = _aggroTarget->reduceByResistance(meleeRule->getPowerBonus(BattleActionAttack::GetBeforeShoot(BA_HIT, _unit, melee)), meleeRule->getDamageType()->ResistType);

	if (dmg > 50)
	{
		meleeOdds += (dmg - 50) / 2;
	}
	if ( _visibleEnemies > 1 )
	{
		meleeOdds -= 20 * (_visibleEnemies - 1);
	}

	if (meleeOdds > 0 && _unit->getHealth() >= 2 * _unit->getBaseStats()->health / 3)
	{
		if (_unit->getAggression() == 0)
		{
			meleeOdds -= 20;
		}
		else if (_unit->getAggression() > 1)
		{
			meleeOdds += 10 * _unit->getAggression();
		}

		if (RNG::percent(meleeOdds))
		{
			_rifle = false;
			_attackAction.weapon = melee;
			bool dummy = false;
			_reachableWithAttack = _save->getPathfinding()->findReachable(_unit, BattleActionCost(BA_HIT, _unit, melee), dummy);
			return;
		}
	}
	_melee = false;
}

/**
 * Checks nearby nodes to see if they'd make good grenade targets
 * @param action contains our details one weapon and user, and we set the target for it here.
 * @return if we found a viable node or not.
 */
bool AIModule::getNodeOfBestEfficacy(BattleAction *action, int radius)
{
	int bestScore = 2;
	Position originVoxel = _save->getTileEngine()->getSightOriginVoxel(_unit);
	Position targetVoxel;
	for (const auto* node : *_save->getNodes())
	{
		if (node->isDummy())
		{
			continue;
		}
		int dist = Position::distance2d(node->getPosition(), _unit->getPosition());
		if (dist <= 20 && dist > radius &&
			_save->getTileEngine()->canTargetTile(&originVoxel, _save->getTile(node->getPosition()), O_FLOOR, &targetVoxel, _unit, false))
		{
			int nodePoints = 0;
			for (auto* bu : *_save->getUnits())
			{
				dist = Position::distance2d(node->getPosition(), bu->getPosition());
				if (!bu->isOut() && dist < radius)
				{
					Position targetOriginVoxel = _save->getTileEngine()->getSightOriginVoxel(bu);
					if (_save->getTileEngine()->canTargetTile(&targetOriginVoxel, _save->getTile(node->getPosition()), O_FLOOR, &targetVoxel, bu, false))
					{
						if ((_unit->getFaction() == FACTION_HOSTILE && bu->getFaction() != FACTION_HOSTILE) ||
							(_unit->getFaction() == FACTION_NEUTRAL && bu->getFaction() == FACTION_HOSTILE))
						{
							if (bu->getTurnsSinceSpotted() <= _intelligence)
							{
								nodePoints++;
							}
						}
						else
						{
							nodePoints -= 2;
						}
					}
				}
			}
			if (nodePoints > bestScore)
			{
				bestScore = nodePoints;
				action->target = node->getPosition();
			}
		}
	}
	return bestScore > 2;
}

BattleUnit* AIModule::getTarget()
{
	return _aggroTarget;
}

void AIModule::freePatrolTarget()
{
	if (_toNode)
	{
		_toNode->freeNode();
	}
}

bool AIModule::visibleToAnyFriend(BattleUnit* target) const
{
	return target->getTurnsSinceSeen(_unit->getFaction()) == 0;
}

void AIModule::brutalThink(BattleAction* action)
{
	// Step 1: Check whether we wait for someone else on our team to move first
	int myReachable = _reachable.size();
	float myDist = 0;
	bool IAmMindControlled = false;
	if (_unit->getFaction() != _unit->getOriginalFaction())
		IAmMindControlled = true;
	Position myPos = _unit->getPosition();
	for (BattleUnit *enemy : *(_save->getUnits()))
	{
		if (enemy->getMainHandWeapon() == NULL || enemy->isOut() || enemy->getFaction() == _unit->getFaction())
			continue;
		Position enemyPos = enemy->getPosition();
		if (!_unit->isCheatOnMovement())
		{
			enemyPos = _save->getTileCoords(enemy->getTileLastSpotted(_unit->getFaction()));
		}
		myDist += Position::distance(myPos, enemyPos);
	}

	for (BattleUnit *ally : *(_save->getUnits()))
	{
		if (ally == _unit)
			continue;
		if (ally->isOut())
			continue;
		if (ally->getFaction() != _unit->getFaction())
			continue;
		if (!ally->reselectAllowed() || !ally->isSelectable(_unit->getFaction(), false, false))
			continue;
		int allyReachable = 0;
		bool allyRanOutOfTUs = false;
		float allyDist = 0;
		bool allyIsMindControlled = false;
		if (ally->getFaction() != ally->getOriginalFaction())
			allyIsMindControlled = true;
		for (BattleUnit *enemy : *(_save->getUnits()))
		{
			if (enemy->getMainHandWeapon() == NULL || enemy->isOut() || enemy->getFaction() == _unit->getFaction())
				continue;
			Position enemyPos = enemy->getPosition();
			if (!_unit->isCheatOnMovement())
			{
				enemyPos = _save->getTileCoords(enemy->getTileLastSpotted(ally->getFaction()));
			}
			allyDist += Position::distance(ally->getPosition(), enemyPos);
		}

		//for testing:
		allyReachable = _save->getPathfinding()->findReachable(ally, BattleActionCost(), allyRanOutOfTUs).size();
		if (_ranOutOfTUs == false)
		{
			if (myReachable < allyReachable)
			{
				action->type = BA_WAIT;
				action->number -= 1;
				_save->getBattleGame()->setNextUnitToSelect(ally);
				return;
			}
		}
		else if (_ranOutOfTUs == true && allyRanOutOfTUs == true)
		{
			if (myDist > allyDist)
			{
				action->type = BA_WAIT;
				action->number -= 1;
				_save->getBattleGame()->setNextUnitToSelect(ally);
				return;
			}
		}
	}

	// Create reachabiliy and turncost-list for the entire map
	if (Options::traceAI)
	{
		Log(LOG_INFO) << "#" << _unit->getId() << "--" << _unit->getType() << " TU: " << _unit->getTimeUnits() << "/" << _unit->getBaseStats()->tu << " Position: " << myPos;
	}

	bool IAmPureMelee = _melee && !_blaster && !_rifle && !_grenade;
	if (_unit->getMainHandWeapon() && _unit->getMainHandWeapon()->getRules()->getBattleType() == BT_MELEE)
		IAmPureMelee = true;
	if (_unit->isLeeroyJenkins())
		IAmPureMelee = true;
	if (IAmPureMelee)
		_attackAction.weapon = _unit->getUtilityWeapon(BT_MELEE);

	bool dummy = false;
	_allPathFindingNodes = _save->getPathfinding()->findReachablePathFindingNodes(_unit, BattleActionCost(), dummy, true);
	int explosionRadius = 0;
	if (_grenade && !_unit->getGrenadeFromBelt()->isFuseEnabled())
	{
		BattleItem *grenade = _unit->getGrenadeFromBelt();
		BattleAction action;
		action.weapon = grenade;
		action.type = BA_THROW;
		action.actor = _unit;
		explosionRadius = grenade->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(action));
	}
	// Phase 3: Check if there's a tile within your range from where you can attack

	BattleUnit *unitToFaceTo = NULL;

	float shortestDist = FLT_MAX;
	int shortestWalkingPath = INT_MAX;
	BattleUnit *unitToWalkTo = NULL;
	int primeScore = 0;
	bool amInAnyonesFOW = false;
	bool amInLoSToFurthestReachable = false;
	bool amCloserThanFurthestReachable = false;

	Position furthestPositionEnemyCanReach = myPos;
	float closestDistanceofFurthestPosition = FLT_MAX;
	bool sweepMode = _unit->isAggressive() || _unit->isLeeroyJenkins();
	int lowestTurnsLastSeen = 255;
	float targetDistanceTofurthestReach = FLT_MAX;
	for (BattleUnit *target : *(_save->getUnits()))
	{
		if (target->isOut())
			continue;
		float primeDist = Position::distance(myPos, target->getPosition());
		if (!isEnemy(target, true))
		{
			if (primeDist <= explosionRadius && target != _unit)
				primeScore -= 2;
			continue;
		}
		if (!_unit->isCheatOnMovement())
		{
			if (target->getTileLastSpotted(_unit->getFaction()) == -1)
			{
				if (target->getFaction() == _targetFaction)
					target->setTileLastSpotted(getClosestSpawnTileId(), _unit->getFaction());
				if (target->getTileLastSpotted(_unit->getFaction()) == -1)
					continue;
			}
		}
		if (primeDist <= explosionRadius)
			primeScore++;
		// Seems redundant but isn't. This is necessary because we also don't want to attack the units that we have mind-controlled
		if (target->getFaction() == _unit->getFaction())
			continue;
		Position targetPosition = target->getPosition();
		int turnsLastSeen = 0;
		if (!_unit->isCheatOnMovement())
		{
			turnsLastSeen = target->getTurnsSinceSeen(_unit->getFaction());
			targetPosition = _save->getTileCoords(target->getTileLastSpotted(_unit->getFaction()));
			Tile *targetTile = _save->getTile(targetPosition);
			bool tileChecked = false;
			if (targetTile->getLastExplored(_unit->getFaction()) == _save->getTurn() && !visibleToAnyFriend(target))
				tileChecked = true;
			else if (targetTile->getUnit() && targetTile->getUnit()->getFaction() == _unit->getFaction())
				tileChecked = true;
			//if (_traceAI)
			//	Log(LOG_INFO) << "Assuming unit at " << target->getPosition() << " to be at " << targetPosition << " checked: " << tileChecked << " target-tile last explored: " << targetTile->getLastExplored(_unit->getFaction()) << " current turn: " << _save->getTurn() << " smoke: " << targetTile->getSmoke() << " turns since seen: " << target->getTurnsSinceSeen(_unit->getFaction());
			if (tileChecked)
			{
				int newIndex = getNewTileIDToLookForEnemy(targetPosition, target);
				if (_traceAI)
				{
					Log(LOG_INFO) << "Target " << target->getPosition() << " is no longer where it is suspected at: " << targetPosition << " Guess for new position is: " << _save->getTileCoords(newIndex);
					//_save->getTile(newIndex)->setMarkerColor(_unit->getId());
					//_save->getTile(newIndex)->setPreview(10);
					//_save->getTile(newIndex)->setTUMarker(target->getId());
				}
				target->setTileLastSpotted(newIndex, _unit->getFaction());
				// We clear it for blind-shot in this case, as it makes no sense to still try and shoot there
				target->setTileLastSpotted(-1, _unit->getFaction(), true);
				if (newIndex == -1)
					continue;
			}
		} else {
			Position blindFirePosition = _save->getTileCoords(target->getTileLastSpotted(_unit->getFaction(), true));
			Tile *targetTile = _save->getTile(blindFirePosition);
			if (targetTile)
			{
				bool tileChecked = false;
				if (targetTile->getLastExplored(_unit->getFaction()) == _save->getTurn() && !visibleToAnyFriend(target))
					tileChecked = true;
				else if (targetTile->getUnit() && targetTile->getUnit()->getFaction() == _unit->getFaction())
					tileChecked = true;
				if (_traceAI)
					Log(LOG_INFO) << "Clearing blind-fire-target for " << target->getPosition() << " previously assumed to be at " << blindFirePosition << " checked: " << tileChecked << " target-tile last explored: " << targetTile->getLastExplored(_unit->getFaction()) << " current turn: " << _save->getTurn() << " smoke: " << targetTile->getSmoke() << " turns since seen: " << target->getTurnsSinceSeen(_unit->getFaction());
				if (tileChecked)
				{
					target->setTileLastSpotted(-1, _unit->getFaction(), true);
				}
			}
		}
		if ((_unit->isCheatOnMovement() || target->getTurnsSinceSeen(_unit->getFaction()) == 0) && hasLofTile(target, _unit->getTile()))
			amInAnyonesFOW = true;
		BattleUnit *LoFCheckUnitForPath = NULL;
		if (_unit->isCheatOnMovement())
			LoFCheckUnitForPath = target;
		int currentWalkPath = tuCostToReachPosition(targetPosition, _allPathFindingNodes);
		Position posUnitCouldReach = closestPositionEnemyCouldReach(target);
		float distToPosUnitCouldReach = Position::distance(myPos, posUnitCouldReach);
		if (distToPosUnitCouldReach < closestDistanceofFurthestPosition)
		{
			furthestPositionEnemyCanReach = posUnitCouldReach;
			closestDistanceofFurthestPosition = distToPosUnitCouldReach;
			targetDistanceTofurthestReach = Position::distance(posUnitCouldReach, targetPosition);
		}
		if (turnsLastSeen < lowestTurnsLastSeen)
		{
			shortestWalkingPath = currentWalkPath;
			lowestTurnsLastSeen = turnsLastSeen;
			unitToWalkTo = target;
		}
		else if (turnsLastSeen == lowestTurnsLastSeen && currentWalkPath < shortestWalkingPath)
		{
			shortestWalkingPath = currentWalkPath;
			unitToWalkTo = target;
		}
	}

	// Phase 1: Check if you can attack anything from where you currently are
	_attackAction.type = BA_RETHINK;
	_psiAction.type = BA_NONE;
	if (brutalPsiAction())
	{
		if (_psiAction.type != BA_NONE)
		{
			action->type = _psiAction.type;
			action->target = _psiAction.target;
			action->number -= 1;
			action->weapon = _psiAction.weapon;
			action->updateTU();
			return;
		}
	}
	if (_blaster)
	{
		brutalBlaster();
	}
	else if (_attackAction.type == BA_RETHINK)
		brutalSelectSpottedUnitForSniper();
	if (_attackAction.type == BA_RETHINK && _grenade)
		brutalGrenadeAction();
	//if (_attackAction.type == BA_RETHINK && _unit->aiTargetMode() >= 3)
	//	blindFire();

	if (_attackAction.type != BA_RETHINK)
	{
		action->type = _attackAction.type;
		action->target = _attackAction.target;
		action->weapon = _attackAction.weapon;
		action->number -= 1;
		if (action->weapon && action->type == BA_THROW && action->weapon->getRules()->getBattleType() == BT_GRENADE && !action->weapon->isFuseEnabled())
		{
			_unit->spendCost(_unit->getActionTUs(BA_PRIME, action->weapon));
			action->weapon->setFuseTimer(0); //don't just spend the TUs for nothing! If we already circumvent the API anyways, we might as well actually prime the damn thing!
			_unit->spendTimeUnits(4);
		}
		action->updateTU();
		if (action->type == BA_LAUNCH)
		{
			action->waypoints = _attackAction.waypoints;
		}
		else if (action->type == BA_AIMEDSHOT || action->type == BA_AUTOSHOT)
		{
			action->kneel = _unit->getArmor()->allowsKneeling(false);
		}
		return;
	}

	// Check if I'm a turret. In this case I can skip everything about walking
	if (!_unit->getArmor()->allowsMoving() || _unit->getEnergy() == 0)
	{
		if (_traceAI)
			Log(LOG_INFO) << "I'm either not allowed to move or have 0 energy. So I'll just end my turn.";
		action->type = BA_NONE;
		setWantToEndTurn(true);
		return;
	}

	Tile *encircleTile = NULL;
	Tile *assumedTile = NULL;
	if (unitToWalkTo != NULL)
	{
		Position targetPosition = unitToWalkTo->getPosition();
		if (!_unit->isCheatOnMovement())
			targetPosition = _save->getTileCoords(unitToWalkTo->getTileLastSpotted(_unit->getFaction()));
		encircleTile = _save->getTile(furthestToGoTowards(targetPosition, BattleActionCost(_unit), _allPathFindingNodes, true));
		if (clearSight(myPos, furthestPositionEnemyCanReach))
			amInLoSToFurthestReachable = true;
		if (closestDistanceofFurthestPosition < _save->getMod()->getMaxViewDistance())
			amCloserThanFurthestReachable = true;
	}
	else if (!_unit->isCheatOnMovement() && _unit->getTimeUnits() == _unit->getBaseStats()->tu)
	{
		if (!encircleTile)
		{
			int LowestTuCost = INT_MAX;
			int lowestTurnExplored = INT_MAX;
			for (auto pu : _allPathFindingNodes)
			{
				Tile *tile = _save->getTile(pu->getPosition());
				if (!tile)
					continue;
				int lastExplored = tile->getLastExplored(_unit->getFaction());
				if (lastExplored < lowestTurnExplored)
				{
					lowestTurnExplored = lastExplored;
					encircleTile = tile;
				}
				else if (lastExplored == lowestTurnExplored)
				{
					int TUCost = tuCostToReachPosition(tile->getPosition(), _allPathFindingNodes);
					if (TUCost < LowestTuCost)
					{
						LowestTuCost = TUCost;
						encircleTile = tile;
					}
				}
			}
		}
	}
	if (encircleTile)
	{
		if (_traceAI)
		{
			Log(LOG_INFO) << "Encircle-Tile: " << encircleTile->getPosition();
			//encircleTile->setMarkerColor(_unit->getId() % 100);
			//encircleTile->setPreview(10);
			//encircleTile->setTUMarker(_unit->getId() %100);
		}
	}
	bool iHaveLof = false;
	bool iHaveLofIncludingEncircle = false;
	bool canReachTargetTileWithAttack = false;
	float myMoralAvg = 0;
	float enemyMoralAvg = 0;
	float myUnitCount = 0;
	float enemyUnitCount = 0;
	BattleActionCost snapCost = BattleActionCost(BA_SNAPSHOT, _unit, action->weapon);
	BattleActionCost hitCost = BattleActionCost(BA_HIT, _unit, action->weapon);
	if (unitToWalkTo != NULL)
	{
		Position targetPosition = unitToWalkTo->getPosition();
		if (!_unit->isCheatOnMovement())
			targetPosition = _save->getTileCoords(unitToWalkTo->getTileLastSpotted(_unit->getFaction()));
		Tile* tileOfTarget = _save->getTile(targetPosition);
		int tuCost = tuCostToReachPosition(targetPosition, _allPathFindingNodes);
		if (IAmPureMelee && _melee)
			canReachTargetTileWithAttack = tuCost <= _unit->getTimeUnits() - BattleActionCost(BA_HIT, _unit, action->weapon).Time;
		else if (snapCost.Time > 0)
			canReachTargetTileWithAttack = tuCost <= _unit->getTimeUnits() - snapCost.Time;
		iHaveLof = quickLineOfFire(myPos, unitToWalkTo, false);
		iHaveLof = iHaveLof || clearSight(myPos, targetPosition);
		iHaveLofIncludingEncircle = iHaveLof;
		if (encircleTile)
		{
			bool sight = clearSight(myPos, encircleTile->getPosition());
			iHaveLofIncludingEncircle = iHaveLofIncludingEncircle || sight;
		}
		for (BattleUnit* teammate : *(_save->getUnits()))
		{
			if (teammate->isOut())
				continue;
			if (teammate->getOriginalFaction() == _unit->getFaction())
			{
				myMoralAvg += teammate->getMorale();
				myUnitCount++;
			}
			else
			{
				enemyMoralAvg += teammate->getMorale();
				enemyUnitCount++;
			}
			if (!teammate->getArmor()->allowsMoving() || teammate->getEnergy() == 0)
				continue;
			if (teammate->getMainHandWeapon() != NULL && teammate->getMainHandWeapon()->getCurrentWaypoints() != 0)
				continue;
		}
		if (myUnitCount > 0)
		{
			myMoralAvg /= myUnitCount;
		}
		if (enemyUnitCount > 0)
		{
			enemyMoralAvg /= enemyUnitCount;
		}
		bool targetHasGravLift = false;
		if (_blaster)
			sweepMode = false;
	}

	BattleActionCost costSnap(BA_SNAPSHOT, _unit, action->weapon);
	bool unarmed = false;
	if (action->weapon == NULL && _grenade == NULL && _unit->getUtilityWeapon(BT_PSIAMP) == NULL)
		unarmed = true;

	if (_unit->getSpecialAbility() == SPECAB_EXPLODEONDEATH || _unit->getSpecialAbility() == SPECAB_BURN_AND_EXPLODE || _unit->isLeeroyJenkins())
	{
		IAmPureMelee = true;
	}
	//When I'm mind-controlled I should definitely be reckless
	if (IAmMindControlled)
	{
		sweepMode = true;
		if (_traceAI)
			Log(LOG_INFO) << "I'm mind-controlled.";
	}

	bool dissolveBlockage = false;
	if (shortestWalkingPath >= 10000 && _unit->getArmor()->getSize() > 1)
	{
		dissolveBlockage = true;
	}
	bool peakMode = false;

	if (_traceAI)
	{
		if (unitToWalkTo)
		{
			Log(LOG_INFO) << "unit with closest walking-distance " << unitToWalkTo->getId() << " " << unitToWalkTo->getPosition() << " dist: " << shortestWalkingPath << " Lof: " << iHaveLof << " can reach target and attack: " << canReachTargetTileWithAttack;
			if (!_unit->isCheatOnMovement())
			{
				Position targetPosition = _save->getTileCoords(unitToWalkTo->getTileLastSpotted(_unit->getFaction()));
				Log(LOG_INFO) << "Since I'm not cheating I think " << unitToWalkTo->getId() << " at " << unitToWalkTo->getPosition() << " is at " << targetPosition;
			}
		}
	}
	// Prio 1: I can walk right ontop of the unit or the unit is already a valid target and I can attack it from where I go
	float bestPrio1Score = 0;
	Position bestPrio1Position = myPos;
	// Prio 2: Find a good spot to approach the enemy but without exposing yourself
	float bestPrio2Score = 0;
	Position bestPrio2Position = myPos;
	// Prio 3: Rush the enemy
	float bestPrio3Score = 0;
	Position bestPrio3Position = myPos;
	// Prio 4: Find a good hiding-spot
	float bestPrio4Score = 0;
	Position bestPrio4Position = myPos;
	float coverInRange = highestCoverInRange(_allPathFindingNodes);
	float tuToSaveForHide = (enemyMoralAvg) / (myMoralAvg + 100);
	if (coverInRange == 0 && iHaveLof)
		sweepMode = true;
	bool shouldSaveEnergy = _unit->getEnergy() + getEnergyRecovery(_unit) < _unit->getBaseStats()->stamina;
	if (myMoralAvg > enemyMoralAvg * 2)
		sweepMode = true;
	if (!shouldSaveEnergy && (!iHaveLof && _unit->getTimeUnits() == getMaxTU(_unit) || canReachTargetTileWithAttack))
		peakMode = true;
	bool shouldSkip = false;
	if (_traceAI)
		Log(LOG_INFO) << "Peak-Mode: " << peakMode << " amInAnyonesFOW: " << amInAnyonesFOW << " iHaveLof: " << iHaveLof << " sweep-mode: " << sweepMode << " too close: " << amCloserThanFurthestReachable << " could be found: " << amInLoSToFurthestReachable << " energy-recovery: " << getEnergyRecovery(_unit) << " coverInRange: " << coverInRange;
	if (Options::allowPreprime && _grenade && !_unit->getGrenadeFromBelt()->isFuseEnabled() && primeScore >= 0 && !IAmMindControlled)
	{
		if (!amInAnyonesFOW && !iHaveLof && (!amCloserThanFurthestReachable || !amInLoSToFurthestReachable) && !canReachTargetTileWithAttack)
		{
			BattleItem *grenade = _unit->getGrenadeFromBelt();
			int primeCost = _unit->getActionTUs(BA_PRIME, grenade).Time + 4;
			if (primeCost <= _unit->getTimeUnits() && !grenade->getRules()->isExplodingInHands())
			{
				_unit->spendTimeUnits(4);
				_unit->spendCost(_unit->getActionTUs(BA_PRIME, grenade));
				grenade->setFuseTimer(0); // don't just spend the TUs for nothing! If we already circumvent the API anyways, we might as well actually prime the damn thing!
				if (_traceAI)
					Log(LOG_INFO) << "I spent " << primeCost << " time-units on priming a grenade because primescore was " << primeScore;
				action->type = BA_RETHINK;
				action->number -= 1;
				return;
			}
		}
	}
	if (!peakMode && !amInAnyonesFOW && !iHaveLof && !sweepMode && !amCloserThanFurthestReachable && !amInLoSToFurthestReachable && getCoverValue(_unit->getTile(), _unit) > coverInRange / 2)
		shouldSkip = true;
	bool winnerWasSpecialDoorCase = false;
	bool shouldHaveLofAfterMove = false;
	int peakDirection = _unit->getDirection();
	if ((unitToWalkTo != NULL || encircleTile) && !shouldSkip)
	{
		Position targetPosition = encircleTile->getPosition();
		bool justNeedToTurn = false;
		std::unordered_set<int> enemyReachable; 
		if (unitToWalkTo)
		{
			targetPosition = unitToWalkTo->getPosition();
			if (!_unit->isCheatOnMovement())
				targetPosition = _save->getTileCoords(unitToWalkTo->getTileLastSpotted(_unit->getFaction()));
			if (myPos != targetPosition && _save->getTileEngine()->getDirectionTo(myPos, targetPosition) != _unit->getDirection() && iHaveLof)
				justNeedToTurn = true;
			enemyReachable = getReachableBy(unitToWalkTo);
		}
		BattleActionCost reserved = BattleActionCost(_unit);
		Position travelTarget = furthestToGoTowards(targetPosition, reserved, _allPathFindingNodes);
		if ((!sweepMode || _blaster) && !(peakMode && !_unit->isCheatOnMovement()))
		{
			Position newTarget = furthestToGoTowards(furthestPositionEnemyCanReach, reserved, _allPathFindingNodes);
			if (newTarget != myPos)
				travelTarget = newTarget;
		}
		std::vector<PathfindingNode *> targetNodes = _save->getPathfinding()->findReachablePathFindingNodes(_unit, BattleActionCost(), dummy, true, NULL, &travelTarget);
		if (_traceAI)
		{
			Log(LOG_INFO) << "travelTarget: " << travelTarget << " targetPositon: " << targetPosition << " peak-mode: " << peakMode << " sweep-mode: " << sweepMode << " furthest-enemy: " << furthestPositionEnemyCanReach << " targetDistanceTofurthestReach: " << targetDistanceTofurthestReach << " need to turn: " << justNeedToTurn << " tuToSaveForHide: " << tuToSaveForHide;
		}
		for (auto pu : _allPathFindingNodes)
		{
			Position pos = pu->getPosition();
			Tile *tile = _save->getTile(pos);
			if (tile == NULL)
				continue;
			if (tile->hasNoFloor() && _unit->getMovementType() != MT_FLY)
				continue;
			if (pu->getTUCost(false).time > _unit->getTimeUnits() || pu->getTUCost(false).energy > _unit->getEnergy())
				continue;
			float closestEnemyDist = FLT_MAX;
			float targetDist = Position::distance(pos, targetPosition);
			bool visibleToEnemy = false;
			bool saveFromGrenades = false;
			float cuddleAvoidModifier = 1;
			bool eaglesCanFly = false;
			bool avoidMeleeRange = false;
			bool lineOfFire = false;
			float closestAnyOneDist = FLT_MAX;
			for (BattleUnit *unit : *(_save->getUnits()))
			{
				if (unit->isOut())
					continue;
				Position unitPosition = unit->getPosition();
				if (!_unit->isCheatOnMovement() && unit->getFaction() != _unit->getFaction())
					unitPosition = _save->getTileCoords(unit->getTileLastSpotted(_unit->getFaction()));
				float unitDist = Position::distance(pos, unitPosition);
				if (isAlly(unit) && unit != _unit && unitPosition.z == pos.z && !IAmMindControlled)
				{
					if (unitDist < 5)
					{
						if (quickLineOfFire(pos, unit))
							cuddleAvoidModifier += 5 - unitDist;
					}
				}
				if (unitDist < closestAnyOneDist && unit != _unit)
					closestAnyOneDist = unitDist;
				if (isAlly(unit))
					continue;
				if (!_unit->isCheatOnMovement() && unit->getTileLastSpotted(_unit->getFaction()) == -1)
					continue;
				if (unit->haveNoFloorBelow())
					eaglesCanFly = true;
				if (shouldAvoidMeleeRange(unit) && unitDist < 2)
				{
					avoidMeleeRange = true;
				}
				if (unitDist < closestEnemyDist)
					closestEnemyDist = unitDist;
				if (hasLofTile(unit, tile) && (brutalValidTarget(unit, true) || _unit->isCheatOnMovement()))
				{
					visibleToEnemy = true;
					if (!IAmPureMelee)
					{
						if (!lineOfFire && brutalValidTarget(unit, true))
						{
							lineOfFire = quickLineOfFire(pos, unit, false, !_unit->isCheatOnMovement());
							if (!_unit->isCheatOnMovement())
								lineOfFire = lineOfFire || clearSight(pos, unitPosition);
							if (lineOfFire)
							{
								if (projectileMayHarmFriends(pos, unitPosition))
									lineOfFire = false;
							}
						}
					}
				}
			}
			bool outOfRangeForShortRangeWeapon = false;
			if (maxExtenderRangeWith(_unit, getMaxTU(_unit)) < closestEnemyDist)
				outOfRangeForShortRangeWeapon = true;
			bool badPath = false;
			if (!isPathToPositionSave(pos))
			{
				badPath = true;
			}
			if (!isPathToPositionSave(pos, true))
				continue;
			bool haveTUToAttack = false;
			if (targetDist < closestEnemyDist)
				closestEnemyDist = targetDist;
			int attackTU = snapCost.Time;
			if (IAmPureMelee) //We want to go in anyways, regardless of whether we still can attack or not
				attackTU = hitCost.Time;
			if ((pos != myPos || justNeedToTurn))
			{
				if (!IAmPureMelee && unitToWalkTo && (brutalValidTarget(unitToWalkTo, true)))
				{
					lineOfFire = quickLineOfFire(pos, unitToWalkTo, false, !_unit->isCheatOnMovement());
					if (!_unit->isCheatOnMovement())
						lineOfFire = lineOfFire || clearSight(pos, targetPosition);
					if (lineOfFire)
					{
						if (projectileMayHarmFriends(pos, targetPosition) && !IAmPureMelee)
							lineOfFire = false;
					}
				}
				if (lineOfFire == false || IAmPureMelee)
				{
					if ((brutalValidTarget(unitToWalkTo, true) || _unit->isCheatOnMovement()) && (_save->getTileEngine()->validMeleeRange(pos, _save->getTileEngine()->getDirectionTo(pos, targetPosition), _unit, unitToWalkTo, NULL) && (_melee || quickLineOfFire(pos, unitToWalkTo, false, !_unit->isCheatOnMovement()))))
					{
						lineOfFire = true;
					}
				}
			}
			bool shouldHaveBeenAbleToAttack = pos == myPos;
			bool realLineOfFire = lineOfFire;
			bool specialDoorCase = false;
			//! Special case: Our target is at a door and the tile we want to go to is too and they have a distance of 1. That means the target is blocking door from other side. So we go there and open it!
			if (!lineOfFire)
			{
				if (_save->getTileEngine()->isNextToDoor(tile) && targetDist == 1)
				{
					Tile *targetTile = _save->getTile(targetPosition);
					if (_save->getTileEngine()->isNextToDoor(targetTile) || IAmPureMelee)
					{
						shouldHaveBeenAbleToAttack = false;
						lineOfFire = true;
						realLineOfFire = false;
						attackTU += 4;
						specialDoorCase = true;
					}
				}
			}
			if (pu->getTUCost(false).time <= _unit->getTimeUnits() - attackTU)
				haveTUToAttack = true;
			float prio1Score = 0;
			float prio2Score = 0;
			float prio3Score = 0;
			float prio4Score = 0;
			if (!_blaster && lineOfFire && haveTUToAttack && (!shouldHaveBeenAbleToAttack || justNeedToTurn))
			{
				if (maxExtenderRangeWith(_unit, _unit->getTimeUnits() - pu->getTUCost(false).time) >= targetDist || IAmPureMelee)
				{
					prio1Score = _unit->getTimeUnits() - pu->getTUCost(false).time;
				}
			}
			Tile *tileAbove = _save->getAboveTile(tile);
			if (tileAbove && !tileAbove->hasNoFloor())
			{
				saveFromGrenades = true;
			}
			if (tile->hasNoFloor())
				saveFromGrenades = true;
			float walkToDist = 20 + tuCostToReachPosition(pos, targetNodes);
			bool clearSightToEnemyReachableTile = false;
			if (furthestPositionEnemyCanReach != myPos)
			{
				if (clearSight(furthestPositionEnemyCanReach, pos))
					clearSightToEnemyReachableTile = true;
			}
			if (encircleTile && !clearSightToEnemyReachableTile)
			{
				if (clearSight(encircleTile->getPosition(), pos))
					clearSightToEnemyReachableTile = true;
			}
			bool closerThanEnemyCanReach = false;
			if (Position::distance(pos, furthestPositionEnemyCanReach) < _save->getMod()->getMaxViewDistance())
				closerThanEnemyCanReach = true;
			float cover = getCoverValue(tile, _unit);
			if (cover * 2 > coverInRange && !realLineOfFire && !sweepMode)
			{
				if (enemyReachable.find(_save->getTileIndex(pos)) == enemyReachable.end() || pos == myPos)
					prio2Score = 100 / walkToDist;
			}
			if ((peakMode && Position::distanceSq(myPos, pos) == 1) || sweepMode)
				prio3Score = 100 / walkToDist;
			prio4Score = closestEnemyDist;
			if (saveFromGrenades)
				prio4Score *= 2;
			if (!lineOfFire)
				prio4Score *= 4;
			if (!visibleToEnemy)
				prio4Score *= 2;
			if (!clearSightToEnemyReachableTile)
				prio4Score *= 4;
			prio4Score *= 1 + 0.25 * cover;
			// If the enemy has produced smoke for us and can't fly, we like being in smoke
			if (tile->getSmoke() > 0 && !eaglesCanFly)
			{
				float smokeMod = 1.0 + tile->getSmoke() * 0.02;
				prio4Score *= smokeMod;
			}
			prio2Score /= cuddleAvoidModifier;
			prio3Score /= cuddleAvoidModifier;
			prio4Score /= cuddleAvoidModifier;
			if (tile->getDangerous() || tile->getFire())
			{
				if (IAmMindControlled && !(tile->getFloorSpecialTileType() == START_POINT && _unit->getOriginalFaction() == FACTION_PLAYER))
				{
					prio1Score *= 2;
					prio2Score *= 10;
					prio3Score *= 10;
					prio4Score *= 10;
				}
				else
				{
					prio1Score /= 2;
					if (_unit->getTile()->getDangerous() || _unit->getTile()->getFire())
					{
						prio2Score /= 10;
						prio3Score /= 10;
						prio4Score /= 10;
					}
					else
					{
						prio2Score = 0;
						prio3Score = 0;
						prio4Score = 0;
					}
				}
			}
			// Avoid tiles from which the player can take me with them when retreating
			if (IAmMindControlled && tile->getFloorSpecialTileType() == START_POINT && _unit->getOriginalFaction() == FACTION_PLAYER)
			{
				prio1Score /= 2;
				prio2Score /= 10;
				prio3Score /= 10;
				prio4Score /= 10;
			}
			if (avoidMeleeRange || (badPath && !sweepMode))
			{
				prio1Score /= 10;
				prio2Score /= 10;
				prio3Score /= 10;
			}
			if (prio1Score > bestPrio1Score)
			{
				bestPrio1Score = prio1Score;
				bestPrio1Position = pos;
				shouldHaveLofAfterMove = realLineOfFire;
				winnerWasSpecialDoorCase = specialDoorCase;
			}
			if (prio2Score > bestPrio2Score)
			{
				bestPrio2Score = prio2Score;
				bestPrio2Position = pos;
			}
			if (prio3Score > bestPrio3Score)
			{
				bestPrio3Score = prio3Score;
				bestPrio3Position = pos;
				if (pu->getPrevNode())
					peakDirection = _save->getTileEngine()->getDirectionTo(pu->getPrevNode()->getPosition(), pos);
			}
			if (prio4Score > bestPrio4Score)
			{
				bestPrio4Score = prio4Score;
				bestPrio4Position = pos;
			}
			//if (_traceAI)
			//{
			//	tile->setMarkerColor(_unit->getId());
			//	tile->setPreview(10);
			//	tile->setTUMarker(cover * 10);
			//}
		}
		if (_traceAI)
		{
			if (bestPrio1Score > 0)
			{
				Log(LOG_INFO) << "bestPrio1Position: " << bestPrio1Position << " score: " << bestPrio1Score;
			}
			if (bestPrio2Score > 0)
			{
				Log(LOG_INFO) << "bestPrio2Position: " << bestPrio2Position << " score: " << bestPrio2Score;
			}
			if (bestPrio3Score > 0)
			{
				Log(LOG_INFO) << "bestPrio3Position: " << bestPrio3Position << " score: " << bestPrio3Score;
			}
			if (bestPrio4Score > 0)
			{
				Log(LOG_INFO) << "bestPrio4Position: " << bestPrio4Position << " score: " << bestPrio4Score;
			}
		}
	}
	Position travelTarget = myPos;
	if (bestPrio1Score > 0)
	{
		travelTarget = bestPrio1Position;
	}
	else if (bestPrio2Score > 0 && (myPos != bestPrio2Position || _unit->getTimeUnits() < getMaxTU(_unit)) && !sweepMode)
	{
		travelTarget = bestPrio2Position;
	}
	else if (bestPrio3Score > 0)
	{
		if (_save->getTileEngine()->visibleTilesFrom(_unit, bestPrio3Position, peakDirection, true).size() > 0 || sweepMode)
			travelTarget = bestPrio3Position;
	}
	else if (bestPrio4Score > 0)
	{
		travelTarget = bestPrio4Position;
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Brutal-AI wants to go from "
					  << myPos
					  << " to travel-target: " << travelTarget << " Remaining TUs: " << _unit->getTimeUnits() << " TU-cost: " << tuCostToReachPosition(travelTarget, _allPathFindingNodes);
	}
	if (travelTarget != myPos)
	{
		BattleActionCost reserved = BattleActionCost(_unit);
		action->target = furthestToGoTowards(travelTarget, reserved, _allPathFindingNodes);
	} else
	{
		tryToPickUpGrenade(_unit->getTile(), action);
		action->target = myPos;
	}
	
	if (_traceAI)
	{
		Log(LOG_INFO) << "Brutal-AI final goto-position from "
					  << myPos
					  << " to " << action->target;
	}
	shortestDist = 255;
	shouldHaveLofAfterMove |= winnerWasSpecialDoorCase;
	for (BattleUnit *target : *(_save->getUnits()))
	{
		if (!isEnemy(target, true) || target->isOut())
			continue;
		if (!_unit->isCheatOnMovement() && target->getTileLastSpotted(_unit->getFaction()) == -1)
			continue;
		Position targetPosition = target->getPosition();
		if (!_unit->isCheatOnMovement())
			targetPosition = _save->getTileCoords(target->getTileLastSpotted(_unit->getFaction()));
		if (shouldHaveLofAfterMove)
		{
			bool haveLof = quickLineOfFire(action->target, target, false, !_unit->isCheatOnMovement()) || shouldHaveLofAfterMove;
			if (!_unit->isCheatOnMovement())
				haveLof = haveLof || clearSight(action->target, targetPosition);
			if (!haveLof)
				continue;
		}
		float currentDist = Position::distance(action->target, targetPosition);
		if (currentDist < shortestDist)
		{
			shortestDist = currentDist;
			unitToFaceTo = target;
		}
	}
	if (_traceAI && unitToFaceTo)
		Log(LOG_INFO) << "unit with closest distance after moving " << unitToFaceTo->getId() << " " << unitToFaceTo->getPosition() << " dist: " << shortestDist;
	action->type = BA_WALK;
	action->finalFacing = -1;
	if (unitToFaceTo != NULL)
	{
		Position targetPosition = unitToFaceTo->getPosition();
		if (!_unit->isCheatOnMovement())
			targetPosition = _save->getTileCoords(unitToFaceTo->getTileLastSpotted(_unit->getFaction()));
		action->finalFacing = _save->getTileEngine()->getDirectionTo(action->target, targetPosition);
		if (_traceAI)
		{
			Log(LOG_INFO) << "Should face towards " << targetPosition << " which is " << action->finalFacing << " should have Lof after move: " << shouldHaveLofAfterMove << " winnerWasSpecialDoorCase: " << winnerWasSpecialDoorCase;
		}
	}
	if (!shouldHaveLofAfterMove)
	{
		if (unitToWalkTo != NULL)
		{
			if (_traceAI)
				Log(LOG_INFO) << "Should look at path towards " << unitToWalkTo->getPosition();
			Position targetPosition = unitToWalkTo->getPosition();
			if (!_unit->isCheatOnMovement())
				targetPosition = _save->getTileCoords(unitToWalkTo->getTileLastSpotted(_unit->getFaction()));
			bool dummy = false;
			std::vector<PathfindingNode *> myNodes = _save->getPathfinding()->findReachablePathFindingNodes(_unit, BattleActionCost(), dummy, true, NULL, &action->target);
			//Tile *lookAtTile = _save->getTile(furthestToGoTowards(targetPosition, BattleActionCost(_unit), myNodes, false, NULL));
			Tile *lookAtTile = _save->getTile(closestToGoTowards(targetPosition, myNodes));
			if (lookAtTile && _traceAI)
				Log(LOG_INFO) << "lookAtTile " << lookAtTile->getPosition() << " action->target: " << action->target;
			if (lookAtTile && lookAtTile->getPosition() != action->target)
			{
				action->finalFacing = _save->getTileEngine()->getDirectionTo(action->target, lookAtTile->getPosition());
				encircleTile = lookAtTile;
				if (_traceAI)
					Log(LOG_INFO) << "Facing corrected towards " << lookAtTile->getPosition() << " which is " << action->finalFacing;
			}
		}
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "My facing now is " << _unit->getDirection() << " and will be " << action->finalFacing;
	}
	action->updateTU();
	if (action->target == myPos)
	{
		if (action->finalFacing != _unit->getDirection() && action->finalFacing != -1)
		{
			action->type = BA_TURN;
			if (unitToFaceTo != NULL)
			{
				Position targetPosition = unitToFaceTo->getPosition();
				if (!_unit->isCheatOnMovement())
					targetPosition = _save->getTileCoords(unitToFaceTo->getTileLastSpotted(_unit->getFaction()));
				action->target = targetPosition;
			}
			if (!iHaveLof && encircleTile && encircleTile->getPosition() != myPos && !winnerWasSpecialDoorCase)
			{
				action->target = encircleTile->getPosition();
			}
			if (_traceAI)
				Log(LOG_INFO) << "Want to turn towards " << action->target;
		}
		else
		{
			action->type = BA_NONE;
			if (unitToFaceTo != NULL)
			{
				Position targetPosition = unitToFaceTo->getPosition();
				if (!_unit->isCheatOnMovement())
					targetPosition = _save->getTileCoords(unitToFaceTo->getTileLastSpotted(_unit->getFaction()));
				action->target = targetPosition;
			}
			if (!iHaveLof && encircleTile && encircleTile->getPosition() != myPos && !winnerWasSpecialDoorCase)
				action->target = encircleTile->getPosition();
			if (_traceAI)
			{
				Log(LOG_INFO) << "Want to end my turn.";
			}
		}
	}
	else
	{
		action->number -= 1;
	}
}

void AIModule::xcommandAIthink(BattleAction* action)
{
	if (_unit->getWantToEndTurn())
		return;

	struct SimpleTile
	{
		Position pos;
		bool discovered, traversable = true, door = false;
		SimpleTile(Tile* tile)
		{
			pos = tile->getPosition();
			discovered = tile->isDiscovered(O_FLOOR);
			traversable = !tile->isBigWall() && !tile->getObstacle(O_FLOOR) && !tile->getDangerous();
			door = false;
			for (int i = 0; i < O_MAX; i++)
			{
				if (tile->isDoor((TilePart)i))
				{
					door = true;
					break;
				}
			}
		}
	};
	struct SimpleAlly
	{
		Position pos;
		int timeUnits;
		struct {
			int health, armorTotal, energy, reaction, morale, mana;
		} stats;
		bool isMindControlled = false, isStunned = false;
		int mainWeapon;
		bool needReload = false;
		SimpleAlly(BattleUnit* ally)
		{
			pos = ally->getPosition();
			timeUnits = ally->getTimeUnits();
			stats.health = ally->getHealth();
			for (int i = 0; i < SIDE_MAX; i++)
				stats.armorTotal += ally->getArmor()->getArmor((UnitSide)i);
			stats.energy = ally->getEnergy();
			stats.reaction = ally->getReactionScore();
			stats.morale = ally->getMorale();
			stats.mana = ally->getMana();
			isMindControlled = ally->getFaction() != ally->getOriginalFaction();
			isStunned = ally->getStunlevel() > 0;
			mainWeapon = ally->getMainHandWeapon()->getId();
			needReload = !ally->getMainHandWeapon()->haveAnyAmmo();
		}
	};
	struct SimpleEnemy
	{
		Position pos;
		std::string type;
		bool canTarget;
		SimpleEnemy(BattleUnit *enemy, bool targetable = false)
		{
			pos = enemy->getPosition();
			type = enemy->getType();
			canTarget = targetable;
		}
	};

	// AI Observation Space
	struct AIobservations
	{
		int currentUnit = 0;
		int currentTurn = 0;
		struct
		{
			std::vector<SimpleTile> byTile;
			int discovered = 0;
		} tiles;
		std::vector<SimpleAlly> allies;
		std::vector<SimpleEnemy> enemies;
		AIobservations(){};
		AIobservations(int id, int turn, std::vector<SimpleTile> tileInfo, std::vector<SimpleAlly> allyInfo, std::vector<SimpleEnemy> enemyInfo)
		{
			currentUnit = id;
			currentTurn = turn;
			tiles.discovered = 0;
			for (SimpleTile& tile : tileInfo)
			{
				tiles.byTile.push_back(tile);
				if (tile.discovered)
					tiles.discovered++;
			}
			for (SimpleAlly& ally : allyInfo)
				allies.push_back(ally);
			for (SimpleEnemy& enemy : enemyInfo)
				enemies.push_back(enemy);
		};
	};

	// Computational State Space
	struct AIstate
	{
	};

	// Combine the different parts of the POMDP
	struct AIinfo 
	{
		AIstate state;
		int actions[2];
			// 0 = end turn, no addtl param
			// 1 = move, 2nd param inidcates direction
			// 2 = attack, 2nd param is id of target
		AIobservations observations;
		float reward() {
			return 0.0;
		}
		AIinfo(AIobservations obsv) {
			observations = obsv;
		}
	};

	// Gather AI Info
	int id = _unit->getId();
	int turn = _save->getTurn();
	Position pos = _unit->getPosition();
	int hp = _unit->getHealth();
	bool mindControlled = _unit->getFaction() != _unit->getOriginalFaction();
	int t = _save->getTurn();
	std::vector<SimpleTile> tiles;
	for (int i = 0; i < _save->getMapSizeXYZ(); i++)
	{
		Tile* t = _save->getTile(i);
		if (!t || !t->isDiscovered(O_FLOOR))
			continue;
		tiles.push_back(SimpleTile(t));
	}
	std::vector<SimpleAlly> allies;
	std::vector<SimpleEnemy> enemies;
	for (BattleUnit* unit : *(_save->getUnits()))
	{
		if (unit->getMainHandWeapon() == NULL || unit->isOut())
			continue;
		if (unit->getFaction() != _unit->getFaction())
		{
			if (visibleToAnyFriend(unit))
				enemies.push_back(SimpleEnemy(unit, validTarget(unit, true, false)));
		}
		else
		{
			allies.push_back(SimpleAlly(unit));
		}
	}

	AIinfo aiInfo = AIinfo(AIobservations(id, turn, tiles, allies, enemies));
	for (int action = -1; action != 0;)
	{
		// send AIinfo to SPN or other policy

		// process action response


		break;
	};
	_unit->setWantToEndTurn(true);
	return;
}

/**
 * Selects a target from a list of units seen by any unit for out-of-LOS actions and populates _attackAction with the relevant data
 * @return True if we have a target selected
 */
bool AIModule::brutalSelectSpottedUnitForSniper()
{
	_aggroTarget = 0;
	// Create a list of spotted targets and the type of attack we'd like to use on each
	std::vector<std::pair<BattleUnit *, BattleAction> > spottedTargets;

	BattleActionCost costThrow;
	// We know we have a grenade, now we need to know if we have the TUs to throw it
	costThrow.type = BA_THROW;
	costThrow.actor = _attackAction.actor;
	costThrow.weapon = _unit->getGrenadeFromBelt();
	costThrow.updateTU();
	if (costThrow.weapon && !costThrow.weapon->isFuseEnabled())
	{
		costThrow.Time += 4; // Vanilla TUs for AI picking up grenade from belt
		costThrow += _attackAction.actor->getActionTUs(BA_PRIME, costThrow.weapon);
	}

	std::vector<BattleItem *> weapons;
	if (_attackAction.actor->getRightHandWeapon())
		weapons.push_back(_attackAction.actor->getRightHandWeapon());
	if (_attackAction.actor->getLeftHandWeapon())
		weapons.push_back(_attackAction.actor->getLeftHandWeapon());
	if (_attackAction.actor->getUtilityWeapon(BT_MELEE))
		weapons.push_back(_attackAction.actor->getUtilityWeapon(BT_MELEE));
	if (_attackAction.actor->getSpecialWeapon(BT_FIREARM))
		weapons.push_back(_attackAction.actor->getSpecialWeapon(BT_FIREARM));
	if (_attackAction.actor->getGrenadeFromBelt())
		weapons.push_back(_attackAction.actor->getGrenadeFromBelt());

	float bestScore = 0;
	BattleAction chosenAction = _attackAction;
	BattleUnit* chosenTarget = _aggroTarget;
	for (std::vector<BattleUnit *>::const_iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
	{
		if (brutalValidTarget(*i))
		{
			for (BattleItem *weapon : weapons)
			{
				// Get the TU costs for each available attack type
				_attackAction.weapon = weapon;
				// Reset the others
				_aggroTarget = (*i);
				_attackAction.type = BA_RETHINK;
				_attackAction.target = (*i)->getPosition();
				BattleActionCost costAuto(BA_AUTOSHOT, _attackAction.actor,weapon);
				BattleActionCost costSnap(BA_SNAPSHOT, _attackAction.actor, weapon);
				BattleActionCost costAimed(BA_AIMEDSHOT, _attackAction.actor, weapon);
				BattleActionCost costHit(BA_HIT, _attackAction.actor, weapon);
				float score = brutalExtendedFireModeChoice(costAuto, costSnap, costAimed, costThrow, costHit, true, bestScore);
				if (score > bestScore)
				{
					bestScore = score;
					chosenAction = _attackAction;
					chosenTarget = _aggroTarget;
				}
			}
		}
	}
	_aggroTarget = chosenTarget;
	_attackAction.type = chosenAction.type;
	_attackAction.weapon = chosenAction.weapon;
	_attackAction.target = chosenAction.target;

	if (bestScore == 0)
	{
		_aggroTarget = 0;
		_attackAction.type = BA_RETHINK;
		_attackAction.weapon = _unit->getMainHandWeapon(false);
	}
	return _aggroTarget != 0;
}

int AIModule::tuCostToReachPosition(Position pos, const std::vector<PathfindingNode *> nodeVector, BattleUnit *actor)
{
	float closestDistToTarget = 255;
	int tuCostToClosestNode = 10000;
	Tile *posTile = _save->getTile(pos);
	bool isAtDoor = _save->getTileEngine()->isNextToDoor(posTile);
	if (actor == NULL)
		actor = _unit;
	for (auto pn : nodeVector)
	{
		if (pos == pn->getPosition())
			return pn->getTUCost(false).time;
		Tile *tile = _save->getTile(pn->getPosition());
		if (pos.z != pn->getPosition().z)
			continue;
		if (!posTile->hasNoFloor() && tile->hasNoFloor() && actor->getMovementType() != MT_FLY)
			continue;
		float currDist = Position::distance(pos, pn->getPosition());
		if (currDist < closestDistToTarget)
		{
			closestDistToTarget = currDist;
			tuCostToClosestNode = pn->getTUCost(false).time;
		}
	}
	return tuCostToClosestNode;
}

Position AIModule::furthestToGoTowards(Position target, BattleActionCost reserved, std::vector<PathfindingNode *> nodeVector, bool encircleTileMode, Tile *encircleTile)
{
	//consider time-units we already spent
	reserved.Time = _unit->getTimeUnits() - reserved.Time;
	reserved.Energy = _unit->getEnergy();
	//We need to consider the cost of standing up
	if (_unit->isKneeled())
	{
		reserved.Time -= _unit->getKneelUpCost();
	}
	PathfindingNode *targetNode = NULL;
	int closestDistToTarget = 255;
	for (auto pn : nodeVector)
	{
		if (target == pn->getPosition())
		{
			targetNode = pn;
			break;
		}
		// If we want to get close to the target it must be on the same layer
		if (target.z != pn->getPosition().z)
		{
			if (target.z > pn->getPosition().z)
			{
				Tile *targetTile = _save->getTile(target);
				Tile *tileAbovePathNode = _save->getAboveTile(_save->getTile(pn->getPosition()));
				if (!targetTile->hasNoFloor() && !tileAbovePathNode->hasNoFloor())
					continue;
			}
			if (target.z < pn->getPosition().z)
			{
				Tile *tileAbovetargetTile = _save->getAboveTile(_save->getTile(target));
				Tile *pathNodeTile = _save->getTile(pn->getPosition());
				if (!tileAbovetargetTile->hasNoFloor() && !pathNodeTile->hasNoFloor())
					continue;
			}
		}
		int currDist = Position::distance(target, pn->getPosition());
		if (currDist < closestDistToTarget)
		{
			closestDistToTarget = currDist;
			targetNode = pn;
		}
	}
	if (targetNode != NULL)
	{
		if (encircleTileMode)
		{
			PathfindingNode *furthestNodeThatWasDangerous = targetNode;
			while (targetNode->getPrevNode() != NULL)
			{
				bool nodeIsDangerous = false;
				Tile *tile = _save->getTile(targetNode->getPosition());
				for (BattleUnit *unit : *(_save->getUnits()))
				{
					if (unit->isOut())
						continue;
					if (isAlly(unit))
						continue;
					if ((_unit->isCheatOnMovement() || brutalValidTarget(unit, true)) && hasLofTile(unit, tile))
						nodeIsDangerous = true;
					if (!_unit->isCheatOnMovement() && unit->getTileLastSpotted(_unit->getFaction()) >= 0)
					{
						Position unitPos = _save->getTileCoords(unit->getTileLastSpotted(_unit->getFaction()));
						nodeIsDangerous = quickLineOfFire(targetNode->getPosition(), unit, false, !_unit->isCheatOnMovement());
						nodeIsDangerous = nodeIsDangerous || clearSight(targetNode->getPosition(), unitPos);
					}
					if (nodeIsDangerous)
						break;
				}
				if (nodeIsDangerous)
					furthestNodeThatWasDangerous = targetNode;
				targetNode = targetNode->getPrevNode();
			}
			if (furthestNodeThatWasDangerous->getPrevNode() != NULL)
				return furthestNodeThatWasDangerous->getPrevNode()->getPosition();
		}
		else
		{
			bool haveLosToEncircleTile = true;
			if (encircleTile != NULL && _unit->getTimeUnits() == _unit->getBaseStats()->tu && targetNode->getTUCost(false).time <= 8)
				haveLosToEncircleTile = false;
			while ((targetNode->getTUCost(false).time > reserved.Time || targetNode->getTUCost(false).energy > reserved.Energy || (haveLosToEncircleTile && encircleTile != NULL)) && targetNode->getPrevNode() != NULL)
			{
				targetNode = targetNode->getPrevNode();
				if (encircleTile != NULL)
				{
					if (clearSight(targetNode->getPosition(), encircleTile->getPosition()) && (_unit->getTimeUnits() < _unit->getBaseStats()->tu || targetNode->getTUCost(false).time > 8))
						haveLosToEncircleTile = true;
					else
						haveLosToEncircleTile = false;
				}
			}
			return targetNode->getPosition();
		}
	}
	return _unit->getPosition();
}

Position AIModule::closestToGoTowards(Position target, std::vector<PathfindingNode *> nodeVector)
{
	PathfindingNode *targetNode = NULL;
	float closestDistToTarget = 255;
	for (auto pn : nodeVector)
	{
		if (target == pn->getPosition())
		{
			targetNode = pn;
			break;
		}
		// If we want to get close to the target it must be on the same layer
		if (target.z != pn->getPosition().z)
		{
			if (target.z > pn->getPosition().z)
			{
				Tile *targetTile = _save->getTile(target);
				Tile *tileAbovePathNode = _save->getAboveTile(_save->getTile(pn->getPosition()));
				if (!targetTile->hasNoFloor() && !tileAbovePathNode->hasNoFloor())
					continue;
			}
			if (target.z < pn->getPosition().z)
			{
				Tile *tileAbovetargetTile = _save->getAboveTile(_save->getTile(target));
				Tile *pathNodeTile = _save->getTile(pn->getPosition());
				if (!tileAbovetargetTile->hasNoFloor() && !pathNodeTile->hasNoFloor())
					continue;
			}
		}
		float currDist = Position::distance(target, pn->getPosition());
		if (currDist < closestDistToTarget)
		{
			closestDistToTarget = currDist;
			targetNode = pn;
		}
	}
	if (targetNode != NULL)
	{
		while (targetNode->getPrevNode() != NULL && targetNode->getPrevNode()->getPosition() != _unit->getPosition())
		{
			targetNode = targetNode->getPrevNode();
			//if (_traceAI)
			//{
			//	Tile *tile = _save->getTile(targetNode->getPosition());
			//	tile->setMarkerColor(_unit->getId());
			//	tile->setPreview(10);
			//	tile->setTUMarker(targetNode->getPrevDir());
			//}
		}
		return targetNode->getPosition();
	}
	return _unit->getPosition();
}

bool AIModule::isPathToPositionSave(Position target, bool checkForProxies)
{
	PathfindingNode *targetNode = NULL;
	float targetNodeDist = Position::distance(target, _unit->getPosition());
	for (auto pn : _allPathFindingNodes)
	{
		if (target == pn->getPosition())
		{
			targetNode = pn;
			break;
		}
	}
	if (targetNode != NULL)
	{
		bool saveForProxies = true;
		while (targetNode->getPrevNode() != NULL)
		{
			Tile *tile = _save->getTile(targetNode->getPosition());
			if (checkForProxies)
			{
				for (int x = -1; x <= 1; ++x)
					for (int y = -1; y <= 1; ++y)
					{
						Position posToCheck = tile->getPosition();
						posToCheck.x += x;
						posToCheck.y += y;
						Tile *tileToCheck = _save->getTile(posToCheck);
						if (tileToCheck)
						{
							for (BattleItem *item : *(tileToCheck->getInventory()))
							{
								if (item->isFuseEnabled() && item->getRules()->getDamageType()->RandomType != DRT_NONE)
									if (tileToCheck != tile || tileToCheck == tile)
										if (_save->getTileEngine()->horizontalBlockage(tileToCheck, tile, DT_HE) >= item->getRules()->getPower())
											saveForProxies = true;
										else
											saveForProxies = false;
									else
										saveForProxies = false;
							}
						}
					}
			}
			else
			{
				for (BattleUnit *unit : *(_save->getUnits()))
				{
					if (unit->isOut())
						continue;
					if (isAlly(unit))
						continue;
					bool suspectReaction = unit->getReactionScore() > (double)_unit->getBaseStats()->reactions * ((double)(_unit->getTimeUnits() - (double)targetNode->getTUCost(false).time) / (_unit->getBaseStats()->tu));
					if (!_unit->isCheatOnMovement())
					{
						if (visibleToAnyFriend(unit))
							suspectReaction = true;
						else
							suspectReaction = false;
					}
					if (unit->hasVisibleTile(tile) && suspectReaction)
					{
						if (unit->hasVisibleUnit(_unit))
							return false;
						else if (targetNode->getPrevNode())
						{
							Tile *prevTile = _save->getTile(targetNode->getPrevNode()->getPosition());
							if (!_unit->isCheatOnMovement() || unit->hasVisibleTile(prevTile) && unit->getReactionScore() > (double)_unit->getBaseStats()->reactions * ((double)(_unit->getTimeUnits() - (double)targetNode->getPrevNode()->getTUCost(false).time) / (_unit->getBaseStats()->tu)))
								return false;
						}
					}
				}
			}
			targetNode = targetNode->getPrevNode();
		}
		return saveForProxies;
	}
	return false;
}

bool AIModule::brutalPsiAction()
{
	BattleItem *item = _unit->getUtilityWeapon(BT_PSIAMP);
	if (!item)
	{
		return false;
	}

	const int costLength = 3;
	BattleActionCost cost[costLength] =
		{
			BattleActionCost(BA_USE, _unit, item),
			BattleActionCost(BA_PANIC, _unit, item),
			BattleActionCost(BA_MINDCONTROL, _unit, item),
		};
	bool have = false;
	for (int j = 0; j < costLength; ++j)
	{
		if (cost[j].Time > 0)
		{
			cost[j].Time;
			cost[j].Energy;
			have |= cost[j].haveTU();
		}
	}
	bool LOSRequired = item->getRules()->isLOSRequired();

	_aggroTarget = 0;
	BattleUnit *bestPsiTarget = NULL;
	float highestPsiScore = 0;

	if (have)
	{
		BattleActionType typeToAttack = BA_NONE;
		for (std::vector<BattleUnit *>::const_iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
		{
			// don't target tanks
			if ((*i)->getArmor()->getSize() == 1 &&
				// they must be armed
				(*i)->getMainHandWeapon() &&
				(!LOSRequired ||
				 std::find(_unit->getVisibleUnits()->begin(), _unit->getVisibleUnits()->end(), *i) != _unit->getVisibleUnits()->end()) &&
				brutalValidTarget(*i, true, true)
				)
			{
				BattleUnit *victim = (*i);
				if (item->getRules()->isOutOfRange(_unit->distance3dToUnitSq(victim)))
				{
					continue;
				}
				// No need to use psi against units that are already panicking or mind-controlled (MindControllerId is also set for panic)
				if (victim->getStatus() == STATUS_PANICKING || victim->getStatus() == STATUS_BERSERK)
				{
					continue;
				}
				for (int j = 0; j < costLength; ++j)
				{
					// can't use this attack.
					if (!cost[j].haveTU())
					{
						continue;
					}
					float psiActionScore = _save->getTileEngine()->psiAttackCalculate({cost[j].type, _unit, item, item}, victim);

					// low chance we hit this target.
					if (psiActionScore < 0)
					{
						continue;
					}
					// when we rolled a 55 or higher on our test-attempt, we are guaranteed to hit which maximizes the successMod's impact on the final score
					psiActionScore = std::min(psiActionScore, 55.0f) / 55.0f;

					// For everyone unit this unit can see we increase the score, regardless of faction
					Position origin = _save->getTileEngine()->getSightOriginVoxel(victim);
					Position targetReference;
					for (BattleUnit *target : *(_save->getUnits()))
					{
						if (target->isOut())
							continue;
						if (Position::distance2d(victim->getPosition(), target->getPosition()) > _save->getMod()->getMaxViewDistance())
							continue;
						if (_save->getTileEngine()->canTargetUnit(&origin, target->getTile(), &targetReference, victim, false, target))
							psiActionScore += 0.1;
					}

					// different bonus per attack.
					if (cost[j].type == BA_MINDCONTROL)
					{
						// target cannot be mind controlled
						if (victim->getUnitRules() && !victim->getUnitRules()->canBeMindControlled())
							continue;
					}
					else if (cost[j].type == BA_PANIC)
					{
						// target cannot be panicked
						if (victim->getUnitRules() && !victim->getUnitRules()->canPanic())
							continue;
						psiActionScore *= std::min(victim->getMorale(), 110 - victim->getBaseStats()->bravery) / 100.0;
					}
					if (psiActionScore > highestPsiScore)
					{
						highestPsiScore = psiActionScore;
						bestPsiTarget = victim;
						typeToAttack = cost[j].type;
					}
				}
			}
		}
		if (bestPsiTarget != NULL)
		{
			_aggroTarget = bestPsiTarget;
			_psiAction.type = typeToAttack;
		}
		if (!_aggroTarget)
			return false;

		if (_traceAI)
		{
			Log(LOG_INFO) << "making a psionic attack against " << _aggroTarget->getId();
		}
		_psiAction.target = _aggroTarget->getPosition();
		_psiAction.weapon = item;
		return true;
	}
	return false;
}

float AIModule::brutalExtendedFireModeChoice(BattleActionCost &costAuto, BattleActionCost &costSnap, BattleActionCost &costAimed, BattleActionCost &costThrow, BattleActionCost &costHit, bool checkLOF, float previousHighScore)
{
	std::vector<BattleActionType> attackOptions = {};
	if (!_unit->isLeeroyJenkins())
	{
		if (costAimed.haveTU())
		{
			attackOptions.push_back(BA_AIMEDSHOT);
		}
		if (costAuto.haveTU())
		{
			attackOptions.push_back(BA_AUTOSHOT);
		}
		if (costSnap.haveTU())
		{
			attackOptions.push_back(BA_SNAPSHOT);
		}
		if (costThrow.haveTU())
		{
			attackOptions.push_back(BA_THROW);
		}
	}
	if (costHit.haveTU())
	{
		attackOptions.push_back(BA_HIT);
	}

	BattleActionType chosenAction = BA_RETHINK;
	BattleAction testAction = _attackAction;
	BattleAction chosenBattleAction = _attackAction;
	float score = previousHighScore;
	Position originPosition = _unit->getPosition();
	//first check our actions from the current tile
	for (auto &i : attackOptions)
	{
		testAction.type = i;
		float newScore = brutalScoreFiringMode(&testAction, _aggroTarget, checkLOF);

		if (newScore > score)
		{
			score = newScore;
			chosenBattleAction.type = i;
			chosenBattleAction.weapon = _attackAction.weapon;
		}
	}
	//Now lets check them from the furtest tile each action can be performed from
	for (auto &i : attackOptions)
	{
		testAction.type = i;
		Position simulationPosition = furthestToGoTowards(_attackAction.target, testAction, _allPathFindingNodes, false);
		Tile *simulationTile = _save->getTile(simulationPosition);
		for (auto &j : attackOptions)
		{
			testAction.type = j;
			float newScore = brutalScoreFiringMode(&testAction, _aggroTarget, checkLOF, simulationTile);

			if (newScore > score && simulationPosition != _unit->getPosition())
			{
				score = newScore;
				chosenBattleAction.type = BA_WALK;
				chosenBattleAction.target = simulationPosition;
				chosenBattleAction.weapon = _attackAction.weapon;
				chosenBattleAction.finalFacing = _save->getTileEngine()->getDirectionTo(simulationPosition, _attackAction.target);
			}
		}
	}
	_attackAction = chosenBattleAction;
	return score;
}

/**
 * Scores a firing mode for a particular target based on a damage / TUs ratio
 * @param action Pointer to the BattleAction determining the firing mode
 * @param target Pointer to the BattleUnit we're trying to target
 * @param checkLOF Set to true if you want to check for a valid line of fire
 * @return The calculated score
 */
float AIModule::brutalScoreFiringMode(BattleAction *action, BattleUnit *target, bool checkLOF, Tile* simulationTile)
{
	// Sanity check first, if the passed action has no type or weapon, return 0.
	if (!action->type || !action->weapon)
	{
		return 0;
	}

	// Get base accuracy for the action
	float accuracy = BattleUnit::getFiringAccuracy(BattleActionAttack::GetBeforeShoot(*action), _save->getBattleGame()->getMod());

	Position originPosition = _unit->getPosition();
	if (simulationTile)
		originPosition = simulationTile->getPosition();
	int distanceSq = Position::distanceSq(originPosition, target->getPosition());
	if (!checkLOF)
		distanceSq = Position::distanceSq(originPosition, _save->getTileCoords(target->getTileLastSpotted(_unit->getFaction(), true)));
	int distance = (int)std::ceil(sqrt(float(distanceSq)));

	int tuTotal = _unit->getTimeUnits();
	float dangerMod = 1;

	if (simulationTile)
	{
		tuTotal -= tuCostToReachPosition(simulationTile->getPosition(), _allPathFindingNodes);
		if (!isPathToPositionSave(simulationTile->getPosition()) || simulationTile->getDangerous() || simulationTile->getFire())
			dangerMod /= 2;
		if (!isPathToPositionSave(simulationTile->getPosition(), true))
			return 0;
	}

	if (Options::battleUFOExtenderAccuracy && action->type != BA_THROW)
	{
		int upperLimit;
		if (action->type == BA_AIMEDSHOT)
		{
			upperLimit = action->weapon->getRules()->getAimRange();
		}
		else if (action->type == BA_AUTOSHOT)
		{
			upperLimit = action->weapon->getRules()->getAutoRange();
		}
		else
		{
			upperLimit = action->weapon->getRules()->getSnapRange();
		}
		int lowerLimit = action->weapon->getRules()->getMinRange();

		if (distance > upperLimit)
		{
			accuracy -= (distance - upperLimit) * action->weapon->getRules()->getDropoff();
		}
		else if (distance < lowerLimit)
		{
			accuracy -= (lowerLimit - distance) * action->weapon->getRules()->getDropoff();
		}
	}

	if (action->type != BA_THROW && action->weapon->getRules()->isOutOfRange(distanceSq))
		accuracy = 0;
	if (action->type == BA_HIT)
	{
		// We can definitely assume we'll be facing the target
		int directionToLook = _save->getTileEngine()->getDirectionTo(originPosition, target->getPosition());
		if (checkLOF)
		{
			if (!_save->getTileEngine()->validMeleeRange(originPosition, directionToLook, _unit, target, 0))
				accuracy = 0;
		}
		else if (distance >= 2)
		{
			accuracy = 0;
		}
	}
	else if (shouldAvoidMeleeRange(target) && distance < 2)
	{
		accuracy = 0;
	}

	float numberOfShots = 1;
	if (action->type == BA_AIMEDSHOT)
	{
		numberOfShots = action->weapon->getRules()->getConfigAimed()->shots;
	}
	else if (action->type == BA_SNAPSHOT)
	{
		numberOfShots = action->weapon->getRules()->getConfigSnap()->shots;
	}
	else if (action->type == BA_AUTOSHOT)
	{
		numberOfShots = action->weapon->getRules()->getConfigAuto()->shots;
	}
	else if (action->type == BA_HIT)
	{
		numberOfShots = action->weapon->getRules()->getConfigMelee()->shots;
	}

	int tuCost = _unit->getActionTUs(action->type, action->weapon).Time;
	tuTotal -= getTurnCostTowards(action->target);
	// Need to include TU cost of getting grenade from belt + priming if we're checking throwing
	float damage = 0;
	if (action->type == BA_THROW && _grenade && action->weapon == _unit->getGrenadeFromBelt())
	{
		if (target->getTile()->getDangerous())
			return 0;
		if (!_unit->getGrenadeFromBelt()->isFuseEnabled())
		{
			tuCost += 4;
			tuCost += _unit->getActionTUs(BA_PRIME, _unit->getGrenadeFromBelt()).Time;
		}
		// We don't have several shots but we can hit several targets at once
		BattleItem *grenade = action->weapon;
		auto radius = grenade->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(*action));
		if (checkLOF)
			numberOfShots = brutalExplosiveEfficacy(target->getPosition(), _unit, radius, true);
		else
			numberOfShots = brutalExplosiveEfficacy(_save->getTileCoords(target->getTileLastSpotted(_unit->getFaction(), true)), _unit, radius, true);
	}
	else
	{
		if (action->type == BA_THROW && action->weapon != _unit->getGrenadeFromBelt())
			return 0;
		auto ammo = action->weapon->getAmmoForAction(action->type);
		if (ammo)
		{
			damage = ammo->getRules()->getPower();
			int radius = ammo->getRules()->getExplosionRadius({action->type, _unit, _attackAction.weapon, ammo});
			if (radius > 0)
				numberOfShots *= brutalExplosiveEfficacy(target->getPosition(), _unit, radius, false);
		}
	}
	// I had to make it mutually exclusive from ammo-damage because that way I wouldn't have power from lasers twice. This seems okay for vanilla but might be wrong for other stuff.
	if (action->weapon->getRules()->getPowerBonus(BattleActionAttack::GetBeforeShoot(*action)))
		damage = action->weapon->getRules()->getPowerBonus(BattleActionAttack::GetBeforeShoot(*action));
	float relevantArmor = 0;
	if (action->type == BA_THROW)
	{
		relevantArmor = target->getArmor()->getUnderArmor();
	}
	else
	{
		relevantArmor = target->getArmor()->getFrontArmor();
	}
	float damageRange = 1.0 + _save->getMod()->DAMAGE_RANGE / 100.0;
	damage = (damage * damageRange - relevantArmor) / 2.0f;
	damage = std::max(damage, 1.0f);
	if (target->getTile() && target->getTile()->getDangerous())
		damage /= 2.0f;

	// Return a score of zero if this firing mode doesn't exist for this weapon
	if (!tuCost)
	{
		return 0;
	}

	numberOfShots *= tuTotal / tuCost;
	if (numberOfShots < 1)
		return 0;

	accuracy /= 100;
	if (accuracy > 0)
		accuracy += std::max(1 - accuracy, 0.0f) / distance;
	accuracy = std::min(1.0f, accuracy);

	Position origin = _save->getTileEngine()->getOriginVoxel((*action), simulationTile);
	Position targetPosition;
	if (action->type != BA_HIT) //Melee-attacks have their own validity check. This additional check can cause false negatives!
	{
		if (checkLOF)
		{
			if (action->weapon->getArcingShot(action->type) || action->type == BA_THROW)
			{
				if (!validateArcingShot(action, simulationTile))
				{
					return 0;
				}
			}
			else
			{
				if (!_save->getTileEngine()->canTargetUnit(&origin, target->getTile(), &targetPosition, _unit, false, target))
				{
					return 0;
				}
				if (projectileMayHarmFriends(action->actor->getPosition(), target->getPosition()))
					return 0;
			}
		}
		else
		{
			if (action->weapon->getArcingShot(action->type) || action->type == BA_THROW)
			{
				if (!validateArcingShot(action, simulationTile))
				{
					return 0;
				}
			}
			else
			{
				if (!clearSight(_unit->getPosition(), targetPosition) || !quickLineOfFire(_unit->getPosition(), target, true, true))
				{
					return 0;
				}
				if (projectileMayHarmFriends(action->actor->getPosition(), target->getPosition()))
					return 0;
			}
		}
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << action->weapon->getRules()->getName() << " damage: " << damage << " armor: " << relevantArmor << " accuracy : " << accuracy << " numberOfShots : " << numberOfShots << " tuCost : " << tuCost << " tuTotal: "<< tuTotal
					  << " from: " << originPosition << " to: "<<action->target
					  << " distance: " << distance << " dangerMod: " << dangerMod
					  << " score: " << damage * accuracy * numberOfShots;
	}
	return damage * accuracy * numberOfShots * dangerMod;
}

/**
 * Decides if it worth our while to create an explosion here.
 * @param targetPos The target's position.
 * @param attackingUnit The attacking unit.
 * @param radius How big the explosion will be.
 * @param diff Game difficulty.
 * @param grenade Is the explosion coming from a grenade?
 * @return Value greater than zero if it is worthwhile creating an explosion in the target position. Bigger value better target.
 */
float AIModule::brutalExplosiveEfficacy(Position targetPos, BattleUnit *attackingUnit, int radius, bool grenade) const
{
	Tile *targetTile = _save->getTile(targetPos);
	if (targetTile->getDangerous())
		return 0;

	// don't throw grenades at flying enemies.
	if (grenade && targetPos.z > 0 && targetTile->hasNoFloor(_save))
	{
		return false;
	}

	int distance = Position::distance2d(attackingUnit->getPosition(), targetPos);
	float enemiesAffected = 0;

	// don't go kamikaze unless we're already doomed.
	if (abs(attackingUnit->getPosition().z - targetPos.z) <= Options::battleExplosionHeight && distance <= radius)
	{
		if (_unit->getFaction() == _unit->getOriginalFaction())
		{
			enemiesAffected -= float(radius - distance / 2.0) / float(radius);
		}
		else
			enemiesAffected += float(radius - distance / 2.0) / float(radius);
	}

	// account for the unit we're targetting
	BattleUnit *target = targetTile->getUnit();
	if (target)
	{
		if (isEnemy(target))
			enemiesAffected++;
		else if (isAlly(target))
			enemiesAffected--;
	}

	for (std::vector<BattleUnit *>::iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
	{
		// don't grenade dead guys
		if (!(*i)->isOut() &&
			// don't count ourself twice
			(*i) != attackingUnit &&
			// don't count the target twice
			(*i) != target &&
			// don't count units that probably won't be affected cause they're out of range
			abs((*i)->getPosition().z - targetPos.z) <= Options::battleExplosionHeight &&
			Position::distance2d((*i)->getPosition(), targetPos) <= radius)
		{
			// don't count people who were already grenaded this turn
			if ((*i)->getTile()->getDangerous())
				continue;

			// trace a line from the grenade origin to the unit we're checking against
			Position voxelPosA = Position(targetPos.toVoxel() + TileEngine::voxelTileCenter);
			Position voxelPosB = Position((*i)->getPosition().toVoxel() + TileEngine::voxelTileCenter);
			std::vector<Position> traj;
			int collidesWith = _save->getTileEngine()->calculateLineVoxel(voxelPosA, voxelPosB, false, &traj, target, *i);

			float dist = Position::distance2d(targetPos, (*i)->getPosition());
			float distMod = float(radius - dist / 2.0) / float(radius);
			if (collidesWith == V_UNIT && traj.front().toTile() == (*i)->getPosition())
			{
				if (isEnemy(*i))
				{
					enemiesAffected += distMod;
				}
				else if (isAlly(*i))
					enemiesAffected--;
			}
		}
	}
	if (_traceAI)
	{
		Log(LOG_INFO) << "Explosion at " << targetPos << " will hit " << enemiesAffected;
	}
	return enemiesAffected;
}

/**
 * Returns whether we think we'd have a line of fire from a particular positon towards a particular target
 * @param pos Positon to check
 * @param target target to check whether we'd have a line of fire
 * @return whether it's likely there would be a line of fire
 */
bool AIModule::quickLineOfFire(Position pos, BattleUnit* target, bool beOkayWithFriendOfTarget, bool lastLocationMode, bool fleeMode) {
	Tile *tile = _save->getTile(pos);
	Position originVoxel = pos.toVoxel() + TileEngine::voxelTileCenter;
	originVoxel.z -= tile->getTerrainLevel();
	Position targetPosition = target->getPosition();
	if (lastLocationMode)
	{
		if (target->getTileLastSpotted(_unit->getFaction()) == -1)
			return false;
		targetPosition = _save->getTileCoords(target->getTileLastSpotted(_unit->getFaction()));
	}
	BattleUnit *unitToIgnore = _unit;
	if (tile->getUnit() && isAlly(tile->getUnit()))
		unitToIgnore = tile->getUnit();
	// In fleeMode we don't ignore ourselves because otherwise we think we can take cover behind ourselves
	if (fleeMode && pos != _unit->getPosition())
		unitToIgnore = NULL;
	for (int x = 0; x < target->getArmor()->getSize(); ++x)
		for (int y = 0; y < target->getArmor()->getSize(); ++y)
		{
			Position targetVoxel = targetPosition;
			targetVoxel += Position(x, y, 0);
			Tile *targetTile = _save->getTile(targetVoxel);
			if (!targetTile)
				return false;
			targetVoxel = targetVoxel.toVoxel();
			targetVoxel += TileEngine::voxelTileCenter;
			targetVoxel.z -= targetTile->getTerrainLevel();
			std::vector<Position> trajectory;
			if (_save->getTileEngine()->calculateLineVoxel(originVoxel, targetVoxel, false, &trajectory, unitToIgnore, NULL, false) == V_UNIT)
			{
				if (targetVoxel.toTile() == trajectory.begin()->toTile())
					return true;
				if (beOkayWithFriendOfTarget && _save->getTile(trajectory.begin()->toTile())->getUnit() && _save->getTile(trajectory.begin()->toTile())->getUnit()->getFaction() == target->getFaction())
					return true;
			}
		}
	return false;
}

/**
 * Returns whether there's clear sight between two positions
 * @param pos Positon to check
 * @param target target to check towards
 * @return whether there is clear sight
 */
bool AIModule::clearSight(Position pos, Position target)
{
	Tile *tile = _save->getTile(pos);
	Tile *targetTile = _save->getTile(target);
	Position originVoxel = pos.toVoxel() + TileEngine::voxelTileCenter;
	originVoxel.z -= tile->getTerrainLevel();
	Position targetVoxel = target.toVoxel() + TileEngine::voxelTileCenter;
	targetVoxel.z -= targetTile->getTerrainLevel();
	std::vector<Position> trajectory;
	if (_save->getTileEngine()->calculateLineVoxel(originVoxel, targetVoxel, false, &trajectory, _unit, NULL, false) == V_EMPTY)
		return true;
	return false;
}

/**
 * Returns the amount of TUs required to turn into a specific direction
 * @param target Positon to consider how many TUs it takes to turn towards
 * @return amount of TUs required to turn in that direction
 */
int AIModule::getTurnCostTowards(Position target)
{
	int currDir = _unit->getFaceDirection();
	int wantDir = _save->getTileEngine()->getDirectionTo(_unit->getPosition(), target);
	int turnSteps = std::abs(currDir - wantDir);
	if (turnSteps > 4)
		turnSteps = 8 - turnSteps;
	return turnSteps *= _unit->getArmor()->getTurnCost();
}

/**
 * Fires a waypoint projectile at an enemy we, or one of our teammates sees.
 *
 * Waypoint targeting: pick from any units currently spotted by our allies.
 */
void AIModule::brutalBlaster()
{
	BattleActionCost attackCost(BA_LAUNCH, _unit, _attackAction.weapon);
	if (!attackCost.haveTU())
	{
		// cannot make a launcher attack - consider some other behaviour, like running away, or standing motionless.
		return;
	}
	_aggroTarget = 0;
	float highestScore = 0;
	for (std::vector<BattleUnit *>::const_iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end() && _aggroTarget == 0; ++i)
	{
		if ((*i)->isOut() || !brutalValidTarget(*i, true))
			continue;
		bool dummy = false;
		std::vector<PathfindingNode *> path = _save->getPathfinding()->findReachablePathFindingNodes(_unit, BattleActionCost(), dummy, true, *i);
		bool havePath = false;
		for (auto node : path)
		{
			if (node->getPosition() == (*i)->getPosition())
				havePath = true;
		}
		auto ammo = _attackAction.weapon->getAmmoForAction(BA_LAUNCH);
		float score = brutalExplosiveEfficacy((*i)->getPosition(), _unit, ammo->getRules()->getExplosionRadius({BA_LAUNCH, _unit, _attackAction.weapon, ammo}), false);
		if (havePath &&
			score > highestScore)
		{
			highestScore = score;
			_aggroTarget = *i;
		}
		_save->getPathfinding()->abortPath();
	}
	//consider blind-blastering too
	bool blindMode = false;
	Position blindTarget;
	if (_aggroTarget == 0 && _unit->aiTargetMode() >= 3)
	{
		for (std::vector<BattleUnit *>::const_iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end() && _aggroTarget == 0; ++i)
		{
			if ((*i)->getTileLastSpotted(_unit->getFaction(), true) == -1)
				continue;
			if (!(*i)->isOut() && isEnemy((*i), true) && !brutalValidTarget(*i, true) && (*i)->getTurnsSinceSeen(_unit->getFaction()) < 2)
			{
				Position targetPos = _save->getTileCoords((*i)->getTileLastSpotted(_unit->getFaction(), true));
				bool dummy = false;
				std::vector<PathfindingNode *> path = _save->getPathfinding()->findReachablePathFindingNodes(_unit, BattleActionCost(), dummy, true, *i);
				bool havePath = false;
				for (auto node : path)
				{
					if (node->getPosition() == targetPos)
						havePath = true;
				}
				auto ammo = _attackAction.weapon->getAmmoForAction(BA_LAUNCH);
				float score = brutalExplosiveEfficacy(targetPos, _unit, ammo->getRules()->getExplosionRadius({BA_LAUNCH, _unit, _attackAction.weapon, ammo}), false);
				// for blind-fire an efficacy of 0 is good enough
				if (havePath &&
					score >= highestScore)
				{
					highestScore = score;
					_aggroTarget = *i;
					blindMode = true;
					blindTarget = targetPos;
					if (_traceAI)
					{
						Log(LOG_INFO) << "Blindfire with blaster at " << blindTarget << " would have a score of " << score;
					}
				}
				_save->getPathfinding()->abortPath();
			}
		}
	}

	if (_aggroTarget != 0)
	{
		bool dummy = false;
		std::vector<PathfindingNode *> missilePaths = _save->getPathfinding()->findReachablePathFindingNodes(_unit, BattleActionCost(), dummy, true, _aggroTarget);
		_attackAction.type = BA_LAUNCH;
		_attackAction.updateTU();
		if (!_attackAction.haveTU())
		{
			_attackAction.type = BA_RETHINK;
			return;
		}
		_attackAction.waypoints.clear();
		int PathDirection;
		int CollidesWith;
		int maxWaypoints = _attackAction.weapon->getCurrentWaypoints();
		if (maxWaypoints == -1)
		{
			maxWaypoints = INT_MAX;
		}
		PathfindingNode *targetNode = NULL;
		Position target = _aggroTarget->getPosition();
		if (blindMode)
			target = blindTarget;
		float closestDistToTarget = 255;
		for (auto pn : missilePaths)
		{
			if (target == pn->getPosition())
			{
				targetNode = pn;
				break;
			}
		}

		if (targetNode != NULL)
		{
			_attackAction.waypoints.push_back(target);
			//If we perform a blind shot we add the final node twice so we hit the ground and not something we might not want to hit
			if (blindMode && blindTarget != _aggroTarget->getPosition())
				_attackAction.waypoints.push_back(target);
			Tile *tile = _save->getTile(target);
			int lastDirection = -1;
			while (targetNode->getPrevNode() != NULL)
			{
				if (targetNode->getPrevNode() != NULL)
				{
					int direction = _save->getTileEngine()->getDirectionTo(targetNode->getPosition(), targetNode->getPrevNode()->getPosition());
					bool zChange = false;
					if (targetNode->getPosition().z != targetNode->getPrevNode()->getPosition().z)
						zChange = true;
					//If we have unlimited way-points for our blaster, we might as well put a way-point on every single node along the path
					if (_attackAction.weapon->getCurrentWaypoints() == -1)
					{
						_attackAction.waypoints.push_front(targetNode->getPosition());
					}
					else if (direction != lastDirection || zChange)
					{
						_attackAction.waypoints.push_front(targetNode->getPosition());
					}
					lastDirection = direction;
				}
				targetNode = targetNode->getPrevNode();
			}
			_attackAction.target = _attackAction.waypoints.front();
			if (_attackAction.waypoints.size() > maxWaypoints)
				_attackAction.type = BA_RETHINK;
			else if (blindMode)
				_aggroTarget->setTileLastSpotted(-1, _unit->getFaction(), true);
		}
		else
		{
			_attackAction.type = BA_RETHINK;
		}
		return;
	}
}

/**
 * Evaluates whether to throw a grenade at an enemy or a tile nearby.
 */
void AIModule::brutalGrenadeAction()
{
	// do we have a grenade on our belt?
	BattleItem *grenade = _unit->getGrenadeFromBelt();
	BattleAction action;
	action.weapon = grenade;
	action.type = BA_THROW;
	action.actor = _unit;

	action.updateTU();
	// Xilmi: Take into account that we might already have primed the grenade before
	if (!action.weapon->isFuseEnabled())
	{
		action.Time += 4; // 4TUs for picking up the grenade
		action += _unit->getActionTUs(BA_PRIME, grenade);
	}
	auto radius = grenade->getRules()->getExplosionRadius(BattleActionAttack::GetBeforeShoot(action));
	Position bestReachablePosition;
	float bestScore = 0;
	for (BattleUnit *target : *(_save->getUnits()))
	{
		if (target->isOut())
			continue;
		if (!brutalValidTarget(target, true))
			continue;
		// We don't want to nade someone who's already been naded
		if (target->getTile() && target->getTile()->getDangerous())
			continue;
		for (int x = 0; x < _save->getMapSizeX(); ++x)
		{
			for (int y = 0; y < _save->getMapSizeY(); ++y)
			{
				Position currentPosition(x, y, target->getPosition().z);
				int dist = Position::distance2d(currentPosition, target->getPosition());
				if (dist <= radius)
				{
					// take into account we might have to turn towards our target
					action.Time += getTurnCostTowards(currentPosition);
					// do we have enough TUs to prime and throw the grenade?
					if (action.haveTU())
					{
						action.target = currentPosition;
						if (!validateArcingShot(&action))
							continue;
						float currentEfficacy = brutalExplosiveEfficacy(currentPosition, _unit, radius, true);
						if (currentEfficacy > bestScore)
						{
							bestReachablePosition = currentPosition;
							bestScore = currentEfficacy;
						}
					}
					action.Time -= getTurnCostTowards(currentPosition);
				}
			}
		}
	}
	if (bestScore > 0)
	{
		_attackAction.weapon = grenade;
		_attackAction.target = bestReachablePosition;
		_attackAction.type = BA_THROW;
		_rifle = false;
		_melee = false;
	}
}

/**
 * Changes whether the AI wants to end their turn
 * @param wantToEndTurn
 */
void AIModule::setWantToEndTurn(bool wantToEndTurn)
{
	_wantToEndTurn = wantToEndTurn;
}

/**
 * Returns whether the AI wants to end their turn
 */
bool AIModule::getWantToEndTurn()
{
	if (!_unit->isBrutal() && _unit->getTurnsSinceStunned() == 0)
		return true;
	return _wantToEndTurn;
}

/**
 * Fires at locations that we've spotted enemies before
 */
void AIModule::blindFire()
{
	// Create a list of spotted targets and the type of attack we'd like to use on each
	std::vector<std::pair<BattleUnit *, BattleAction> > spottedTargets;

	BattleActionCost costThrow;
	// We know we have a grenade, now we need to know if we have the TUs to throw it
	costThrow.type = BA_THROW;
	costThrow.actor = _attackAction.actor;
	costThrow.weapon = _unit->getGrenadeFromBelt();
	costThrow.updateTU();
	if (costThrow.weapon && !costThrow.weapon->isFuseEnabled())
	{
		costThrow.Time += 4; // Vanilla TUs for AI picking up grenade from belt
		costThrow += _attackAction.actor->getActionTUs(BA_PRIME, costThrow.weapon);
	}

	std::vector<BattleItem *> weapons;
	if (_attackAction.actor->getRightHandWeapon())
		weapons.push_back(_attackAction.actor->getRightHandWeapon());
	if (_attackAction.actor->getLeftHandWeapon())
		weapons.push_back(_attackAction.actor->getLeftHandWeapon());
	if (_attackAction.actor->getUtilityWeapon(BT_MELEE))
		weapons.push_back(_attackAction.actor->getUtilityWeapon(BT_MELEE));
	if (_attackAction.actor->getSpecialWeapon(BT_FIREARM))
		weapons.push_back(_attackAction.actor->getSpecialWeapon(BT_FIREARM));
	if (_attackAction.actor->getGrenadeFromBelt())
		weapons.push_back(_attackAction.actor->getGrenadeFromBelt());

	for (std::vector<BattleUnit *>::const_iterator i = _save->getUnits()->begin(); i != _save->getUnits()->end(); ++i)
	{
		if ((*i)->getTileLastSpotted(_unit->getFaction(), true) == -1)
			continue;
		if (!(*i)->isOut() && isEnemy((* i), true) && !brutalValidTarget(*i, true))
		{
			// Determine which firing mode to use based on how many hits we expect per turn and the unit's intelligence/aggression
			_aggroTarget = (*i);
			_attackAction.type = BA_RETHINK;
			_attackAction.target = _save->getTileCoords((*i)->getTileLastSpotted(_unit->getFaction(), true));
			for (BattleItem *weapon : weapons)
			{
				// Get the TU costs for each available attack type
				_attackAction.weapon = weapon;
				BattleActionCost costAuto(BA_AUTOSHOT, _attackAction.actor, weapon);
				BattleActionCost costSnap(BA_SNAPSHOT, _attackAction.actor, weapon);
				BattleActionCost costAimed(BA_AIMEDSHOT, _attackAction.actor, weapon);
				BattleActionCost costHit(BA_HIT, _attackAction.actor, weapon);
				brutalExtendedFireModeChoice(costAuto, costSnap, costAimed, costThrow, costHit, false);

				BattleAction chosenAction = _attackAction;
				if (_attackAction.type != BA_RETHINK)
				{
					std::pair<BattleUnit *, BattleAction> spottedTarget;
					spottedTarget = std::make_pair((*i), chosenAction);
					spottedTargets.push_back(spottedTarget);
				}
			}
		}
	}

	int numberOfTargets = static_cast<int>(spottedTargets.size());

	if (numberOfTargets) // Now that we have a list of valid targets, pick one and return.
	{
		float clostestDist = 255;
		for (auto targetAction : spottedTargets)
		{
			float dist = Position::distance(targetAction.first->getPosition(), _unit->getPosition());
			Tile *targetTile = _save->getTile(targetAction.first->getPosition());
			// deprioritize naded targets but don't ignore them completely
			if (targetTile->getDangerous())
				dist *= 5;
			if (dist < clostestDist)
			{
				clostestDist = dist;
				_aggroTarget = targetAction.first;
				_attackAction.type = targetAction.second.type;
				_attackAction.weapon = targetAction.second.weapon;
				_attackAction.target = _save->getTileCoords(_aggroTarget->getTileLastSpotted(_unit->getFaction(), true));
			}
		}
		if (_aggroTarget)
		{
			if (_traceAI)
				Log(LOG_INFO) << "Blindfire at " << _attackAction.target;
			// we blindFire only once per target, so doing so clears up the remembered position:
			_aggroTarget->setTileLastSpotted(-1, _unit->getFaction(), true);
		}
	}
	else // We didn't find a suitable target
	{
		// Make sure we reset anything we might have changed while testing for targets
		_aggroTarget = 0;
		_attackAction.type = BA_RETHINK;
		_attackAction.weapon = _unit->getMainHandWeapon(false);
	}
}

bool AIModule::validateArcingShot(BattleAction *action, Tile* originTile)
{
	action->actor = _unit;
	if (originTile == NULL)
		originTile = _unit->getTile();
	Position origin = _save->getTileEngine()->getOriginVoxel((*action), originTile);
	Tile *targetTile = _save->getTile(action->target);
	Position targetVoxel;
	std::vector<Position> targets;
	double curvature;
	targetVoxel = action->target.toVoxel() + Position(8, 8, (1 + -targetTile->getTerrainLevel()));
	targets.clear();
	bool forced = false;

	if (action->type == BA_THROW)
	{
		targets.push_back(targetVoxel);
	}
	else
	{
		BattleUnit *tu = targetTile->getOverlappingUnit(_save);
		if (tu && tu->getVisible())
		{                                          // unit
			targetVoxel.z += tu->getFloatHeight(); // ground level is the base
			targets.push_back(targetVoxel + Position(0, 0, tu->getHeight() / 2 + 1));
			targets.push_back(targetVoxel + Position(0, 0, 2));
			targets.push_back(targetVoxel + Position(0, 0, tu->getHeight() - 1));
		}
		else if (targetTile->getMapData(O_OBJECT) != 0)
		{
			targetVoxel = action->target.toVoxel() + Position(8, 8, 0);
			targets.push_back(targetVoxel + Position(0, 0, 13));
			targets.push_back(targetVoxel + Position(0, 0, 8));
			targets.push_back(targetVoxel + Position(0, 0, 23));
			targets.push_back(targetVoxel + Position(0, 0, 2));
		}
		else if (targetTile->getMapData(O_NORTHWALL) != 0)
		{
			targetVoxel = action->target.toVoxel() + Position(8, 0, 0);
			targets.push_back(targetVoxel + Position(0, 0, 13));
			targets.push_back(targetVoxel + Position(0, 0, 8));
			targets.push_back(targetVoxel + Position(0, 0, 20));
			targets.push_back(targetVoxel + Position(0, 0, 3));
		}
		else if (targetTile->getMapData(O_WESTWALL) != 0)
		{
			targetVoxel = action->target.toVoxel() + Position(0, 8, 0);
			targets.push_back(targetVoxel + Position(0, 0, 13));
			targets.push_back(targetVoxel + Position(0, 0, 8));
			targets.push_back(targetVoxel + Position(0, 0, 20));
			targets.push_back(targetVoxel + Position(0, 0, 2));
		}
		else if (targetTile->getMapData(O_FLOOR) != 0)
		{
			targets.push_back(targetVoxel);
		}
	}
	int test = V_OUTOFBOUNDS;
	for (std::vector<Position>::iterator i = targets.begin(); i != targets.end(); ++i)
	{
		targetVoxel = *i;
		if (_save->getTileEngine()->validateThrow((*action), origin, targetVoxel, _save->getDepth(), &curvature, &test, forced))
		{
			return true;
		}
	}
	return false;
}

bool AIModule::brutalValidTarget(BattleUnit *unit, bool moveMode, bool psiMode) const
{
	if (unit == NULL)
		return false;
	if (unit->isOut() || unit->isIgnoredByAI() ||
		isAlly(unit))
	{
		return false;
	}
	bool iAmMindControlled = false;
	if (_unit->getOriginalFaction() != _unit->getFaction())
		iAmMindControlled = true;
	if (_unit->aiTargetMode() < 2 && !moveMode)
	{
		if (_unit->hasVisibleUnit(unit))
			return isEnemy(unit, iAmMindControlled);
		else
			return false;
	}
	else if (_unit->aiTargetMode() < 4 || moveMode)
	{
		if (visibleToAnyFriend(unit))
			return isEnemy(unit, iAmMindControlled);
		else
			return false;
	}
	return isEnemy(unit, iAmMindControlled);
}

Position AIModule::closestPositionEnemyCouldReach(BattleUnit *enemy)
{
	if (!_unit->isCheatOnMovement() && enemy->getTileLastSpotted(_unit->getFaction()) == -1)
		return _unit->getPosition();
	PathfindingNode *targetNode = NULL;
	int tu = 0;
	for (auto pn : _allPathFindingNodes)
	{
		Position enemyPositon = _save->getTileCoords(enemy->getTileLastSpotted(_unit->getFaction()));
		if (_unit->isCheatOnMovement())
			enemyPositon = enemy->getPosition();
		if (enemyPositon == pn->getPosition())
		{
			targetNode = pn;
			tu = pn->getTUCost(false).time;
			break;
		}
	}
	tu -= getMaxTU(enemy);
	if (targetNode != NULL)
	{
		while (targetNode->getPrevNode() != NULL)
		{
			if (targetNode->getTUCost(false).time < tu)
				return targetNode->getPosition();
			targetNode = targetNode->getPrevNode();
		}
	}
	return _unit->getPosition();
}

int AIModule::maxExtenderRangeWith(BattleUnit *unit, int tus)
{
	BattleItem *weapon = unit->getMainHandWeapon();
	if (weapon == NULL)
		return 0;
	if (!Options::battleUFOExtenderAccuracy)
	{
		if (weapon->getRules()->getBattleType() == BT_MELEE)
			return 1;
		return weapon->getRules()->getMaxRange();
	}
	int highestRangeAvailableWithTUs = 0;
	if (weapon->getRules()->getCostAimed().Time > 0 && unit->getActionTUs(BA_AIMEDSHOT, weapon).Time < tus)
		highestRangeAvailableWithTUs = weapon->getRules()->getAimRange();
	if (weapon->getRules()->getCostSnap().Time > 0 && unit->getActionTUs(BA_SNAPSHOT, weapon).Time < tus)
		highestRangeAvailableWithTUs = std::max(highestRangeAvailableWithTUs, weapon->getRules()->getSnapRange());
	if (weapon->getRules()->getCostAuto().Time > 0 && unit->getActionTUs(BA_AUTOSHOT, weapon).Time < tus)
		highestRangeAvailableWithTUs = std::max(highestRangeAvailableWithTUs, weapon->getRules()->getAutoRange());
	if (weapon->getRules()->getCostMelee().Time > 0 && unit->getActionTUs(BA_HIT, weapon).Time < tus)
		highestRangeAvailableWithTUs = std::max(highestRangeAvailableWithTUs, 1);
	highestRangeAvailableWithTUs = std::min(highestRangeAvailableWithTUs, weapon->getRules()->getMaxRange());
	return highestRangeAvailableWithTUs;
}

int AIModule::getNewTileIDToLookForEnemy(Position previousPosition, BattleUnit* unit)
{
	Tile *TileToCheckNext = NULL;
	int LowestTuCost = INT_MAX;
	float maxDistFromLastSeen = unit->getTurnsSinceSeen(_unit->getFaction()) * getMaxTU(unit) / 4;
	Position lastSpottedPositon = _save->getTileCoords(unit->getTileLastSpotted(_unit->getFaction()));
	for (auto pn : _allPathFindingNodes)
	{
		Tile *tile = _save->getTile(pn->getPosition());
		if (unit->getTileLastSpotted(_unit->getFaction()) != -1 && Position::distance(pn->getPosition(), lastSpottedPositon) > maxDistFromLastSeen)
			continue;
		int lastExplored = tile->getLastExplored(_unit->getFaction());
		if (lastExplored < _save->getTurn())
		{
			int TUCost = pn->getTUCost(false).time;
			if (TUCost < LowestTuCost)
			{
				LowestTuCost = TUCost;
				TileToCheckNext = tile;
			}
		}
	}
	if (TileToCheckNext)
		return _save->getTileIndex(TileToCheckNext->getPosition());
	return -1;
}

int AIModule::getMaxTU(BattleUnit *unit)
{
	int maxTU = 0;
	if (!unit->isOut())
	{
		// Add to previous turn TU, if regen is less than normal unit need couple of turns to regen full bar
		maxTU = unit->getBaseStats()->tu;
		float encumbrance = (float)unit->getBaseStats()->strength / (float)unit->getCarriedWeight();
		if (encumbrance < 1)
		{
			maxTU = int(encumbrance * maxTU);
		}
		// Each fatal wound to the left or right leg reduces the soldier's TUs by 10%.
		maxTU -= (maxTU * ((unit->getFatalWound(BODYPART_LEFTLEG) + unit->getFatalWound(BODYPART_LEFTLEG)) * 10)) / 100;
	}
	return maxTU;
}

int AIModule::getClosestSpawnTileId()
{
	int id = -1;
	if (_save->getMissionType() == "STR_BASE_DEFENSE")
		return id;
	for (auto pn : _allPathFindingNodes)
	{
		Position tilePositon = pn->getPosition();
		Tile *tile = _save->getTile(tilePositon);
		if (tile->getFloorSpecialTileType() == START_POINT)
		{
			if (_traceAI)
				Log(LOG_INFO) << "Assuming a target to be at " << tilePositon;
			return _save->getTileIndex(tilePositon);
		}
	}
	return id;
}

bool AIModule::isEnemy(BattleUnit* unit, bool ignoreSameOriginalFaction) const
{
	if (!unit)
		return false;
	if (_unit == unit)
		return false;
	if (unit->isIgnoredByAI())
		return false;
	UnitFaction faction = unit->getFaction();
	bool unitIsMindControlled = false;
	if (unit->getFaction() != unit->getOriginalFaction())
		unitIsMindControlled = true;
	if (ignoreSameOriginalFaction || unitIsMindControlled)
		faction = unit->getOriginalFaction();
	if (_unit->getFaction() == FACTION_HOSTILE)
	{
		if (faction == FACTION_PLAYER || faction == FACTION_NEUTRAL)
			return true;
	}
	else if (_unit->getFaction() == FACTION_NEUTRAL)
	{
		if (faction == FACTION_HOSTILE)
			return true;
	}
	else if (_unit->getFaction() == FACTION_PLAYER)
	{
		if (faction == FACTION_HOSTILE)
			return true;
	}
	return false;
}

bool AIModule::isAlly(BattleUnit *unit) const
{
	if (!unit)
		return false;
	UnitFaction faction = unit->getFaction();
	if (_unit->getFaction() == FACTION_HOSTILE)
	{
		if (faction == FACTION_PLAYER || faction == FACTION_NEUTRAL)
			return false;
	}
	else if (_unit->getFaction() == FACTION_NEUTRAL)
	{
		if (faction == FACTION_HOSTILE)
			return false;
	}
	else if (_unit->getFaction() == FACTION_PLAYER)
	{
		if (faction == FACTION_HOSTILE)
			return false;
	}
	return true;
}

bool AIModule::projectileMayHarmFriends(Position startPos, Position targetPos)
{
	float distance = Position::distance(startPos, targetPos);
	Position posToCheck = targetPos;
	std::vector<Position> trajectory;
	trajectory.clear();
	int tst = _save->getTileEngine()->calculateLineTile(startPos, posToCheck, trajectory);
	// Reveal all tiles along line of vision. Note: needed due to width of bresenham stroke.
	for (std::vector<Position>::iterator i = trajectory.begin(); i != trajectory.end(); ++i)
	{
		Position posVisited = (*i);
		if (posVisited == startPos)
			continue;
		Tile *tile = _save->getTile(posVisited);
		if (!tile)
			continue;
		if (tile && tile->getUnit() && isAlly(tile->getUnit()) && !tile->getUnit()->isOut() && tile->getUnit() != _unit)
		{
			return true;
		}
	}
	return false;
}

bool AIModule::inRangeOfAnyFriend(Position pos)
{
	for (BattleUnit* ally : *(_save->getUnits()))
	{
		if (ally->isOut())
			continue;
		if (ally->getFaction() != _unit->getFaction())
			continue;
		if(maxExtenderRangeWith(ally, getMaxTU(ally)) > Position::distance(ally->getPosition(), pos))
			return true;
	}
	return false;
}

bool AIModule::shouldAvoidMeleeRange(BattleUnit *enemy)
{
	if (_melee)
		return false;
	if (_save->getMod()->getEnableCloseQuartersCombat() && !_unit->getArmor()->getIgnoresMeleeThreat() && enemy->getArmor()->getCreatesMeleeThreat())
		return true;
	return false;
}

bool AIModule::isArmed(BattleUnit *unit) const
{
	if (unit->getMainHandWeapon())
		return true;
	if (unit->getGrenadeFromBelt())
		return true;
	if (unit->getUtilityWeapon(BT_PSIAMP))
		return true;
	if (unit->getSpecialWeapon(BT_MELEE))
		return true;
	if (unit->getSpecialWeapon(BT_FIREARM))
		return true;
	return false;
}

bool AIModule::hasLofTile(BattleUnit *unit, Tile *tile)
{
	if (unit->hasLofTile(tile))
		return true;
	if (unit->hasNoLofTile(tile))
		return false;
	std::vector<Position> _trajectory;
	_trajectory.clear();
	int tst = _save->getTileEngine()->calculateLineTile(unit->getPosition(), tile->getPosition(), _trajectory);
	if (tst > 127)
	{
		// Vision impacted something before reaching posTest. Throw away the impact point.
		unit->addToNoLofTiles(tile);
		_trajectory.pop_back();
	}
	// Reveal all tiles along line of vision. Note: needed due to width of bresenham stroke.
	for (std::vector<Position>::iterator i = _trajectory.begin(); i != _trajectory.end(); ++i)
	{
		Position posVisited = (*i);
		if (!unit->hasLofTile(_save->getTile(posVisited)))
		{
			unit->addToLofTiles(_save->getTile(posVisited));
		}
	}
	if (unit->hasLofTile(tile))
		return true;
	return false;
}

void AIModule::tryToPickUpGrenade(Tile *tile, BattleAction *action)
{
	for (BattleItem *item : *(tile->getInventory()))
	{
		if (item->isFuseEnabled())
		{
			if (_save->getBattleGame()->takeItemFromGround(item, action) == 0)
				if (_traceAI)
					Log(LOG_INFO) << "Picked up " << item->getRules()->getName() << " from " << tile->getPosition();
		}
	}
}

float AIModule::getItemPickUpScore(BattleItem* item)
{
	if (!_unit->isBrutal())
		return item->getRules()->getAttraction();
	if (!_save->canUseWeapon(item, _unit, false, BA_SNAPSHOT))
		return 0;
	if (item->haveAnyAmmo())
	{
		if(item->getRules()->getBattleType() == BT_FIREARM)
			return 10;
		if (item->getRules()->getBattleType() == BT_GRENADE || item->getRules()->getBattleType() == BT_MELEE)
			return 5;
	} 
	if (item->getRules()->getBattleType() == BT_AMMO)
	{
		for (const auto *bi : *_unit->getInventory())
		{
			if (bi->getRules()->getBattleType() == BT_FIREARM)
			{
				if (bi->getRules()->getSlotForAmmo(item->getRules()) != -1)
				{
					return 10;
				}
			}
		}
	}
	return 0;
}

bool AIModule::IsEnemyExposedEnough()
{
	bool dummy = false;
	Position startPosition = _unit->getPosition();

	if (getClosestSpawnTileId() >= 0)
		startPosition = _save->getTileCoords(getClosestSpawnTileId());
	else
		return true;
	if (_traceAI)
	{
		Log(LOG_INFO) << "startPos: " << startPosition;
	}
	std::vector<PathfindingNode *> enemySimulationNodes = _save->getPathfinding()->findReachablePathFindingNodes(_unit, BattleActionCost(), dummy, true, NULL, &startPosition);
	for (BattleUnit *enemy : *(_save->getUnits()))
	{
		if (!isEnemy(enemy))
			continue;
		if (enemy->isOut())
			continue;
		if (visibleToAnyFriend(enemy))
			return true;
		Position currentAssumedPosition = _save->getTileCoords(enemy->getTileLastSpotted(_unit->getFaction()));
		int turnsSinceSeen = enemy->getTurnsSinceSeen(_unit->getFaction());
		if (_unit->isCheatOnMovement())
		{
			currentAssumedPosition = enemy->getPosition();
			turnsSinceSeen = 0;
		}
		else if (enemy->getTileLastSpotted(_unit->getFaction()) == -1)
			return false;
		turnsSinceSeen = std::max(turnsSinceSeen, 1);
		int requiredTUFromStart = turnsSinceSeen * getMaxTU(enemy);
		int neededTUToStart = tuCostToReachPosition(currentAssumedPosition, enemySimulationNodes, enemy);
		bool inSmoke = false;
		if (_save->getTile(currentAssumedPosition) && _save->getTile(currentAssumedPosition)->getSmoke() > 0)
			inSmoke = true;
		if (_traceAI)
		{
			Log(LOG_INFO) << enemy->getId() << ", seen " << enemy->getTurnsSinceSeen(_unit->getFaction()) << " turns ago, needs to be at least " << requiredTUFromStart << " TUs from the starting-location. We assume they should need " << neededTUToStart << " in smoke: "<<inSmoke;
		}
		//If I'm in smoke myself, I have the same advantage as the enemy and thus don't care whether they are in smoke
		if (_unit->getTile()->getSmoke() > 0)
			inSmoke = false;
		if (requiredTUFromStart < neededTUToStart && !inSmoke)
			return true;
	}
	return false;
}

float AIModule::getCoverValue(Tile *tile, BattleUnit *bu)
{
	if (tile == NULL)
		return 0;
	float cover = 0;
	Tile* tileFrom = tile;
	int peakOver = tile->getTerrainLevel() * -1 + bu->getHeight() - 24;
	if (peakOver > 0)
		tileFrom = _save->getAboveTile(tile);
	if (tileFrom == NULL)
		tileFrom = tile;
	for (int direction = 0; direction <= 7; ++direction)
	{
		Position posInDirection = tileFrom->getPosition();
		switch (direction)
		{
		case 0:
			posInDirection += Position(0, -1, 0);
			break;
		case 1:
			posInDirection += Position(1, -1, 0);
			break;
		case 2:
			posInDirection += Position(1, 0, 0);
			break;
		case 3:
			posInDirection += Position(1, 1, 0);
			break;
		case 4:
			posInDirection += Position(0, 1, 0);
			break;
		case 5:
			posInDirection += Position(-1, 1, 0);
			break;
		case 6:
			posInDirection += Position(-1, 0, 0);
			break;
		case 7:
			posInDirection += Position(-1, -1, 0);
			break;
		}
		Tile *tileInDirection = _save->getTile(posInDirection);
		if (tileInDirection)
		{
			//A unit is not good cover, especially not if it's yourself
			if (tileInDirection->getUnit())
				continue;
			float totalEnemies = 0;
			float enemiesInThisDirection = 0;
			for (BattleUnit *enemy : *(_save->getUnits()))
			{
				if (!enemy->isOut() && isEnemy(enemy))
				{
					if (!_unit->isCheatOnMovement() && enemy->getTileLastSpotted(_myFaction) == -1)
						continue;
					Position pos = _save->getTileCoords(enemy->getTileLastSpotted(_myFaction));
					if (_unit->isCheatOnMovement())
						pos = enemy->getPosition();
					int enemyDir = _save->getTileEngine()->getDirectionTo(tile->getPosition(), pos);
					float dist = Position::distance(tile->getPosition(), pos);
					if (direction == enemyDir)
					{
						enemiesInThisDirection += 1.0 / dist;
					}
					if (direction == enemyDir - 1 || (direction == 0 && enemyDir == 7))
						enemiesInThisDirection += 0.5 / dist;
					if (direction == enemyDir + 1 || (direction == 7 && enemyDir == 0))
						enemiesInThisDirection += 0.5 / dist;
					totalEnemies += 2.0 / dist;
				}
			}
			float dirCoverMod = enemiesInThisDirection / totalEnemies;
			std::vector<Position> traj;
			float coverFromDir = 0;
			for (int part = O_FLOOR; part < O_MAX; ++part)
			{
				coverFromDir += _save->getTileEngine()->horizontalBlockage(tileInDirection, tileFrom, DT_NONE) / 255.0;
				if (coverFromDir >= 1)
					coverFromDir += _save->getTileEngine()->horizontalBlockage(tileInDirection, tileFrom, DT_HE) / 255.0;
			}
			if (coverFromDir > 0)
				cover += coverFromDir * dirCoverMod;
		}
	}
	return cover;
}

float AIModule::highestCoverInRange(const std::vector<PathfindingNode *> nodeVector)
{
	float highestCover = 0;
	for (auto pn : nodeVector)
	{
		if (pn->getTUCost(false).time > _unit->getTimeUnits() || pn->getTUCost(false).energy > _unit->getEnergy())
			continue;
		Tile *tile = _save->getTile(pn->getPosition());
		float cover = getCoverValue(tile, _unit);
		if (cover > highestCover)
		{
			highestCover = cover;
			//if (_traceAI)
			//{
			//	tile->setMarkerColor(_unit->getId());
			//	tile->setPreview(10);
			//	tile->setTUMarker(10 * getCoverValue(tile, _unit));
			//}
		}
	}
	return highestCover;
}

bool AIModule::isAnyMovementPossible()
{
	bool dummy = true;
	if (_save->getPathfinding()->findReachablePathFindingNodes(_unit, BattleActionCost(), dummy, false, NULL, NULL, true).size() > 1)
		return true;
	return false;
}

int AIModule::getEnergyRecovery(BattleUnit* unit)
{
	int recovery = 0;
	if (unit->getGeoscapeSoldier())
	{
		for (const auto* bonusRule : *(unit->getGeoscapeSoldier()->getBonuses(nullptr)))
		{
			recovery += bonusRule->getEnergyRecovery(unit);
		}
	}
	recovery = _unit->getArmor()->getEnergyRecovery(unit, recovery);
	return recovery;
}

std::unordered_set<int> AIModule::getReachableBy(BattleUnit* unit)
{
	bool dummy;
	Position startPosition = _save->getTileCoords(unit->getTileLastSpotted(_unit->getFaction()));
	if (_unit->isCheatOnMovement())
		startPosition = unit->getPosition();
	std::vector<PathfindingNode*> reachable = _save->getPathfinding()->findReachablePathFindingNodes(unit, BattleActionCost(), dummy, false, NULL, &startPosition, false, true);
	std::unordered_set<int> tiles;
	tiles.reserve(reachable.size());
	for (std::vector<PathfindingNode*>::const_iterator it = reachable.begin(); it != reachable.end(); ++it)
	{
		tiles.insert(_save->getTileIndex((*it)->getPosition()));
	}
	return tiles;
}

}
