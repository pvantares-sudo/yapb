//
// YaPB, started from PODBot by Count Floyd
// Maintained by YaPB Team <yapb@jeefo.net>
//
// SPDX-License-Identifier: Unlicense
//

#include <yapb.h>

namespace bot {

int Bot::NumFriendsNear (const ystl::Vector &origin, const float radius) const {
  if (game.Is (GameFlags::FreeForAll)) {
    return 0; // no friends on free for all mode
  }
  const float radius_sq = ystl::sqrf (radius);

  return static_cast<int> (ystl::count_if (clients, [&] (const auto &client) {
    return client.IsTeammate (team_, Ent ()) && client.IsInRadius (origin, radius_sq);
  }));
}

int Bot::NumEnemiesNear (const ystl::Vector &origin, const float radius) const {
  const float radius_sq = ystl::sqrf (radius);
  const bool free_for_all = game.Is (GameFlags::FreeForAll);

  // on free-for-all every other player is a hostile
  return static_cast<int> (ystl::count_if (clients, [&] (const auto &client) {
    return client.IsUsedAndAlive () && client.ent != Ent () && (free_for_all || !client.IsSameTeam (team_)) &&
           client.IsInRadius (origin, radius_sq);
  }));
}

bool Bot::IsEnemyHidden (edict_t *enemy) {
  if (!cv_check_enemy_rendering || game.IsNullEntity (enemy)) {
    return false;
  }
  const auto &v = enemy->v;

  const bool enemy_has_gun = has_flag (v.weapons, kPrimaryWeaponMask) || has_flag (v.weapons, kSecondaryWeaponMask);
  const bool enemy_gunfire = (v.button & IN_ATTACK) || (v.oldbuttons & IN_ATTACK);

  if ((v.renderfx == kRenderFxExplode || (v.effects & EF_NODRAW)) && (!enemy_gunfire || !enemy_has_gun)) {
    return true;
  }

  if ((v.renderfx == kRenderFxExplode || (v.effects & EF_NODRAW)) && enemy_gunfire && enemy_has_gun) {
    return false;
  }

  if (v.renderfx != kRenderFxHologram && v.renderfx != kRenderFxExplode && v.rendermode != kRenderNormal) {
    if (v.renderfx == kRenderFxGlowShell) {
      if (v.renderamt <= 20.0f && v.rendercolor.x <= 20.0f && v.rendercolor.y <= 20.0f && v.rendercolor.z <= 20.0f) {
        if (!enemy_gunfire || !enemy_has_gun) {
          return true;
        }
        return false;
      }
      else if (!enemy_gunfire && v.renderamt <= 60.0f && v.rendercolor.x <= 60.f && v.rendercolor.y <= 60.0f && v.rendercolor.z <= 60.0f) {
        return true;
      }
    }
    else if (v.renderamt <= 20.0f) {
      if (!enemy_gunfire || !enemy_has_gun) {
        return true;
      }
      return false;
    }
    else if (!enemy_gunfire && v.renderamt <= 60.0f) {
      return true;
    }
  }
  return false;
}

bool Bot::IsEnemyInvincible (edict_t *enemy) {
  if (!cv_check_enemy_invincibility || game.IsNullEntity (enemy)) {
    return false;
  }
  const auto &v = enemy->v;

  if (v.solid < SOLID_BBOX) {
    return true;
  }

  if (v.flags & FL_GODMODE) {
    return true;
  }

  if (ystl::fequal (v.takedamage, DAMAGE_NO)) {
    return true;
  }
  return false;
}

bool Bot::IsEnemyNoTarget (edict_t *enemy) {
  if (game.IsNullEntity (enemy)) {
    return false;
  }
  return !!(enemy->v.flags & FL_NOTARGET);
}

bool Bot::IsEnemyInDarkArea (edict_t *enemy) const {
  if (!cv_check_darkness || game.IsNullEntity (enemy)) {
    return false;
  }
  const auto enemy_node_index = graph.GetNearest (enemy->v.origin);

  if (!graph.Exists (enemy_node_index)) {
    return false;
  }
  const auto light_level = graph[enemy_node_index].light;

  if (light_level > 30.0f) {
    return false;
  }

  if (uses_nvg_ || (enemy->v.effects & EF_DIMLIGHT)) {
    return false;
  }
  const auto sky_color = illum.GetSkyColor ();
  const bool is_very_dark = (light_level < 3.0f && sky_color > 50.0f) || (light_level < 25.0f && sky_color <= 50.0f);

  if (!is_very_dark) {
    return false;
  }

  const bool has_weapon = !!(enemy->v.weapons & (kPrimaryWeaponMask | kSecondaryWeaponMask));
  const bool is_attacking = (enemy->v.button & IN_ATTACK) || (enemy->v.oldbuttons & IN_ATTACK);

  return !(is_attacking && has_weapon);
}

bool Bot::IsEnemyInDarkAreaCached (edict_t *enemy) {
  if (game.IsNullEntity (enemy)) {
    return false;
  }
  if (dark_area_check_timer_.elapsed () || dark_area_check_enemy_ != enemy ||
      dark_area_check_origin_.distance_sq (enemy->v.origin) > ystl::sqrf (64.0f)) {
    dark_area_check_enemy_ = enemy;
    dark_area_check_origin_ = enemy->v.origin;
    dark_area_result_ = IsEnemyInDarkArea (enemy);
    dark_area_check_timer_.start (0.1f);
  }
  return dark_area_result_;
}

bool Bot::CheckBodyParts (edict_t *target) {
  const bool dark_hidden = target != enemy_ && IsEnemyInDarkAreaCached (target);

  if (IsEnemyHidden (target) || IsEnemyInvincible (target) || IsEnemyNoTarget (target) || dark_hidden) {
    enemy_parts_ = Visibility::None;
    enemy_origin_.clear ();

    return false;
  }

  if (game.Is (GameFlags::HasStudioModels) && cv_use_hitbox_enemy_targeting && hitbox_enumerator_) {
    return CheckBodyPartsWithHitboxes (target);
  }
  return CheckBodyPartsWithOffsets (target);
}

bool Bot::CheckBodyPartsWithOffsets (edict_t *target) {
  Trace::Result result {};
  const ystl::Vector eyes = GetEyesPos ();

  auto spot = target->v.origin;
  auto self = Ent ();

  const auto ignore_flags = is_creature_ ? TraceIgnore::None : (cv_aim_trace_consider_glass ? TraceIgnore::Monsters : TraceIgnore::Everything);

  const auto hits_target = [&] () -> bool {
    if (result.hit == target) {
      return true;
    }
    if (result.fraction >= 1.0f) {
      return trace.IsEndpointClear (result);
    }
    return false;
  };

  enemy_parts_ = Visibility::None;
  trace.Line (eyes, spot, ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Body;
    enemy_origin_ = result.end_pos;
  }

  spot.z += 25.0f;
  trace.Line (eyes, spot, ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Head;
    enemy_origin_ = result.end_pos;
  }

  if (enemy_parts_ != Visibility::None) {
    return true;
  }

  constexpr auto kStandFeet = 34.0f;
  constexpr auto kCrouchFeet = 14.0f;
  constexpr auto kEdgeOffset = 13.0f;

  if (target->v.flags & FL_DUCKING) {
    spot.z = target->v.origin.z - kCrouchFeet;
  }
  else {
    spot.z = target->v.origin.z - kStandFeet;
  }
  trace.Line (eyes, spot, ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Other;
    enemy_origin_ = result.end_pos;

    return true;
  }
  ystl::Vector dir = (target->v.origin - pev->origin).normalize2d ();

  ystl::Vector perp (-dir.y, dir.x, 0.0f);
  spot = target->v.origin + ystl::Vector (perp.x * kEdgeOffset, perp.y * kEdgeOffset, 0.0f);

  trace.Line (eyes, spot, ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Other;
    enemy_origin_ = result.end_pos;

    return true;
  }
  spot = target->v.origin - ystl::Vector (perp.x * kEdgeOffset, perp.y * kEdgeOffset, 0.0f);

  trace.Line (eyes, spot, ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Other;
    enemy_origin_ = result.end_pos;

    return true;
  }
  return false;
}

bool Bot::CheckBodyPartsWithHitboxes (edict_t *target) {
  const auto self = Ent ();
  const auto refresh = frame_interval_ * 1.5f;

  Trace::Result result {};
  const ystl::Vector eyes = GetEyesPos ();

  const auto hits_target = [&] () -> bool {
    if (result.hit == target) {
      return true;
    }
    if (result.fraction >= 1.0f) {
      return trace.IsEndpointClear (result);
    }
    return false;
  };
  enemy_parts_ = Visibility::None;

  const auto ignore_flags = is_creature_ ? TraceIgnore::None : TraceIgnore::Everything;

  trace.Line (eyes, hitbox_enumerator_->Get (target, PlayerPart::Stomach, refresh), ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Body;
    enemy_origin_ = result.end_pos;
  }

  trace.Line (eyes, hitbox_enumerator_->Get (target, PlayerPart::Head, refresh), ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Head;
    enemy_origin_ = result.end_pos;
  }

  if (enemy_parts_ != Visibility::None) {
    return true;
  }

  trace.Line (eyes, hitbox_enumerator_->Get (target, PlayerPart::LeftArm, refresh), ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Other;
    enemy_origin_ = result.end_pos;

    return true;
  }

  trace.Line (eyes, hitbox_enumerator_->Get (target, PlayerPart::RightArm, refresh), ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Other;
    enemy_origin_ = result.end_pos;

    return true;
  }

  trace.Line (eyes, hitbox_enumerator_->Get (target, PlayerPart::Feet, refresh), ignore_flags, self, &result);

  if (hits_target ()) {
    enemy_parts_ |= Visibility::Other;
    enemy_origin_ = result.end_pos;

    return true;
  }
  return false;
}

bool Bot::SeesEnemy (edict_t *player) {
  auto is_behind_smoke_clouds = [&] (const ystl::Vector &pos) {
    if (cv_smoke_grenade_checks.As<int> () == 2) {
      return IsLineBlockedBySmoke (GetEyesPos (), pos);
    }
    return false;
  };

  if (game.IsNullEntity (player)) {
    return false;
  }
  bool ignore_field_of_view = false;

  if (cv_whose_your_daddy && game.IsPlayerEntity (pev->dmg_inflictor) && game.GetPlayerTeam (pev->dmg_inflictor) != team_) {
    ignore_field_of_view = true;
  }

  if ((ignore_field_of_view || IsInViewCone (player->v.origin)) && frustum.Check (view_frustum_, player) &&
      !is_behind_smoke_clouds (player->v.origin) && CheckBodyParts (player)) {
    return true;
  }
  return false;
}

void Bot::TrackEnemies () {
  if (LookupEnemies ()) {
    states_ |= Sense::SeeingEnemy;
  }
  else {
    states_ &= ~Sense::SeeingEnemy;

    enemy_ = nullptr;
    enemy_body_part_set_ = nullptr;
  }

  if (has_flag (states_, Sense::SeeingEnemy) && UsesKnife () && !IsKnifeMode () && HasAnyAmmo () && IsOnFloor ()) {
    auto enemy = enemy_;

    if (!game.IsNullEntity (enemy)) {
      const auto distance_sq = pev->origin.distance_sq (enemy->v.origin);

      if (distance_sq > ystl::sqrf (240.f)) {
        SelectBestWeapon ();
      }
    }
  }
}

float Bot::EnemyMemorySpan () const {
  const float skill = static_cast<float> (Skill ()) / 100.0f;
  const float aggression = ystl::clamp (agression_level_, 0.0f, 1.0f);

  float closeness = 0.0f;

  if (!last_enemy_origin_.empty ()) {
    closeness = ystl::clamp (1.0f - last_enemy_origin_.distance (pev->origin) / 2048.0f, 0.0f, 1.0f);
  }
  return ystl::clamp (3.0f + 3.0f * skill + 1.5f * aggression + 2.0f * closeness, 3.0f, 8.0f);
}

bool Bot::IsInCombat () const {
  if (has_flag (states_, Sense::SeeingEnemy) || wants_to_fire_) {
    return true;
  }
  return !ystl::fzero (last_damage_timestamp_) && game.Time () - last_damage_timestamp_ < 1.5f;
}

float Bot::EngagementScore (edict_t *enemy) const {
  if (game.IsNullEntity (enemy)) {
    return 0.0f;
  }

  const float distance = pev->origin.distance (enemy->v.origin);
  float score = 800.0f / (distance + 128.0f);

  if (enemy->v.button & IN_ATTACK) {
    score += 2.0f;
  }
  if (has_flag (enemy->v.weapons, kSniperWeaponMask)) {
    score += 1.0f;
  }
  if (enemy->v.health <= 35.0f) {
    score += 1.0f;
  }
  return score;
}

int Bot::TeammatesEngaging (edict_t *enemy) const {
  int engaged = 0;

  for (auto &mate : bots) {
    if (&mate != this && mate.is_alive_ && mate.team_ == team_ && mate.enemy_ == enemy) {
      ++engaged;
    }
  }
  return engaged;
}

bool Bot::ShouldSwitchFocus (edict_t *current, edict_t *candidate) const {
  if (game.IsNullEntity (current) || game.IsNullEntity (candidate) || current == candidate) {
    return false;
  }

  if (TeammatesEngaging (candidate) >= 2) {
    return false;
  }

  const float current_value = EngagementScore (current) + 1.5f * static_cast<float> (TeammatesEngaging (current));
  const float candidate_value = EngagementScore (candidate) + 1.5f * static_cast<float> (TeammatesEngaging (candidate)) + 0.5f;

  return candidate_value > current_value * 1.2f;
}

bool Bot::LookupEnemies () {
  enemy_parts_ = Visibility::None;
  enemy_origin_.clear ();

  if (cv_ignore_enemies || !enemy_ignore_timer_.elapsed () || !blind_timer_.elapsed ()) {
    return false;
  }

  // Защита от стрельбы до раунда: если на сервере пока нет ни одного зомби 
  // (у зомби здоровье больше 1500 или нет огнестрельного оружия), врагов не ищем
  bool zombie_found = false;
  for (const auto &client : clients) {
    if (client.IsUsedAndAlive () && client.ent != nullptr) {
      const bool has_guns = has_flag (client.ent->v.weapons, kPrimaryWeaponMask) || has_flag (client.ent->v.weapons, kSecondaryWeaponMask);
      if (client.ent->v.health > 1500.0f || !has_guns) {
        zombie_found = true;
        break;
      }
    }
  }

  if (!zombie_found) {
    return false; // До заражения стрельба и выбор целей полностью отключены
  }

  edict_t *player, *new_enemy = nullptr;
  float nearest_distance_sq = ystl::sqrf (view_distance_);

  if (!game.IsNullEntity (enemy_) && has_flag (states_, Sense::SeeingEnemy)) {
    states_ &= ~Sense::SuspectEnemy;
  }
  else if (game.IsNullEntity (enemy_) && see_enemy_timer_.less_than (EnemyMemorySpan ()) && game.IsAliveEntity (last_enemy_)) {
    states_ |= Sense::SuspectEnemy;

    const bool deny_last_enemy = pev->velocity.length_sq2d () > 0.0f && last_enemy_origin_.distance_sq (pev->origin) < ystl::sqrf (256.0f) &&
                                 shoot_time_ + 1.5f > game.Time ();

    if (!has_flag (aim_flags_, AimFlags::Enemy | AimFlags::PredictPath | AimFlags::Danger) && !deny_last_enemy &&
        SeesEntity (last_enemy_origin_, true)) {

      aim_flags_ |= AimFlags::LastEnemy;
    }
  }

  if (!game.IsNullEntity (enemy_)) {
    player = enemy_;

    if (!enemy_update_timer_.elapsed () && player->v.origin.distance_sq (pev->origin) < nearest_distance_sq && game.IsAliveEntity (player) &&
        SeesEnemy (player)) {

      new_enemy = player;
      enemy_update_timer_.start (UsesKnife () ? 1.25f : 0.85f);
    }
  }

  if (game.IsNullEntity (new_enemy)) {
    uint8_t *set = nullptr;

    if (cv_use_engine_pvs_check) {
      set = game.GetVisibilitySet (this, true);
    }

    edict_t *shield_enemy = nullptr;

    if (cv_attack_monsters) {
      for (const auto &interesting : game_state.GetInterestingEntities ()) {
        if (interesting.kind != EntityKind::Monster) {
          continue;
        }
        const auto ent = interesting.ent;

        if (cv_use_engine_pvs_check && !game.CheckVisibility (ent, set)) {
          continue;
        }

        if (SeesEnemy (ent)) {
          const float scale_factor = (1.0f / CalculateScaleFactor (ent));
          const float distance_sq = ent->v.origin.distance_sq (pev->origin) * scale_factor;

          if (distance_sq < nearest_distance_sq) {
            nearest_distance_sq = distance_sq;
            new_enemy = ent;
          }
        }
      }
    }

    for (const auto &client : clients) {
      if (!client.IsUsedAndAlive () || client.IsSameTeam (team_)) {
        continue;
      }
      player = client.ent;

      const float distance_sq = player->v.origin.distance_sq (pev->origin);

      const bool extended_range = cv_whose_your_daddy && (player->v.button & (IN_ATTACK | IN_ATTACK2)) && view_distance_ < max_view_distance_;
      const float effective_nearest_sq = extended_range ? ystl::sqrf (max_view_distance_) : nearest_distance_sq;

      if (distance_sq >= effective_nearest_sq) {
        continue;
      }

      if (cv_use_engine_pvs_check && !game.CheckVisibility (player, set)) {
        continue;
      }

      if (SeesEnemy (player)) {
        if (IsEnemyBehindShield (player)) {
          shield_enemy = player;
          continue;
        }
        nearest_distance_sq = distance_sq;
        new_enemy = player;

        if (game.MapIs (MapFlags::Assassination) && game.IsPlayerVip (new_enemy)) {
          break;
        }
      }
    }

    if (!game.IsNullEntity (new_enemy)) {
      edict_t *focus = nullptr;
      float focus_score = 0.0f;

      for (auto &mate : bots) {
        if (&mate == this || !mate.is_alive_ || mate.team_ != team_ || game.IsNullEntity (mate.enemy_) || mate.enemy_ == new_enemy) {
          continue;
        }

        if (mate.pev->origin.distance_sq (pev->origin) >= ystl::sqrf (900.0f) || !SeesEnemy (mate.enemy_) ||
            !ShouldSwitchFocus (new_enemy, mate.enemy_)) {
          continue;
        }

        const float score = EngagementScore (mate.enemy_);

        if (focus == nullptr || score > focus_score) {
          focus = mate.enemy_;
          focus_score = score;
        }
      }

      if (focus != nullptr && target_switch_timer_.elapsed ()) {
        new_enemy = focus;
        target_switch_timer_.start (1.5f);
      }
    }
    enemy_update_timer_.start (UsesKnife () ? 1.25f : 0.85f);

    if (game.IsNullEntity (new_enemy) && !game.IsNullEntity (shield_enemy)) {
      new_enemy = shield_enemy;
    }
  }

  if (new_enemy != nullptr && (game.IsPlayerEntity (new_enemy) || (cv_attack_monsters && game.IsMonsterEntity (new_enemy)))) {
    bots.SetEnemySpotted (true);

    aim_flags_ |= AimFlags::Enemy;
    states_ |= Sense::SeeingEnemy;

    if (new_enemy == enemy_) {
      see_enemy_timer_.start ();

      actual_reaction_time_ = 0.0f;
      last_enemy_ = new_enemy;
      last_enemy_origin_ = new_enemy->v.origin;

      bots.ReportTeamContact (team_, new_enemy, new_enemy->v.origin, 1);

      return true;
    }
    else {
      const bool objective_callout = see_enemy_timer_.greater_than (3.0f) && (has_c4_ || has_hostage_ || !game.IsNullEntity (target_entity_));

      if (objective_callout && bots.TryTeamSpeech (team_, 1.5f, SpeechTopic::Enemy, SpeechPriority::Normal)) {
        if (cv_radio_mode.As<int> () == 2) {
          HandleChatterEnemyDown ();
        }
        else if (cv_radio_mode.As<int> () == 1) {
          PushRadioChat (RadioChat::EnemySpotted);
        }
      }
      target_entity_ = nullptr;

      enemy_surprise_timer_.start (cv_whose_your_daddy ? actual_reaction_time_ * 0.5f : actual_reaction_time_);

      actual_reaction_time_ = 0.0f;
      enemy_ = new_enemy;
      last_enemy_ = new_enemy;
      enemy_body_part_set_ = nullptr;
      last_enemy_origin_ = new_enemy->v.origin;
      enemy_reachable_timer_.invalidate ();

      bots.ReportTeamContact (team_, new_enemy, new_enemy->v.origin, NumEnemiesNear (new_enemy->v.origin, 1024.0f));

      if (!objective_callout && bots.TryTeamSpeech (team_, 1.5f, SpeechTopic::Enemy, SpeechPriority::Normal)) {
        if (cv_radio_mode.As<int> () == 2) {
          HandleChatterEnemyDown ();
        }
        else if (cv_radio_mode.As<int> () == 1) {
          PushRadioChat (RadioChat::EnemySpotted);
        }
      }

      see_enemy_timer_.start ();

      if (!(old_buttons_ & IN_ATTACK)) {
        return true;
      }

      for (auto &other : bots) {
        if (!other.is_alive_ || other.team_ != team_ || &other == this) {
          continue;
        }

        if (other.see_enemy_timer_.greater_than (2.0f) && game.IsNullEntity (other.last_enemy_) && util.IsVisible (pev->origin, other.Ent ()) &&
            other.IsInViewCone (pev->origin)) {

          other.last_enemy_ = new_enemy;
          other.last_enemy_origin_ = new_enemy->v.origin;
          other.see_enemy_timer_.start ();
          other.states_ |= (Sense::SuspectEnemy | Sense::HearingEnemy);
          other.aim_flags_ |= AimFlags::LastEnemy;
        }
      }
      return true;
    }
  }
  else if (!game.IsNullEntity (enemy_)) {
    new_enemy = enemy_;
    last_enemy_ = new_enemy;

    if (!game.IsAliveEntity (new_enemy)) {
      enemy_ = nullptr;
      enemy_body_part_set_ = nullptr;

      if (see_enemy_timer_.less_than (0.1f)) {
        if (!UsesSniper ()) {
          shoot_at_dead_timer_.start (ystl::clamp (agression_level_ * 1.25f, 0.15f, 0.25f));
          actual_reaction_time_ = 0.0f;
          states_ |= Sense::SuspectEnemy;

          return true;
        }
        return false;
      }
      else if (!shoot_at_dead_timer_.elapsed ()) {
        actual_reaction_time_ = 0.0f;
        states_ |= Sense::SuspectEnemy;

        return true;
      }
      return false;
    }

    if (cv_shoots_thru_walls && IsPenetrableObstacleCached (new_enemy->v.origin)) {
      auto engage_thru_wall = [&] () {
        thru_wall_hold_timer_.start (rg (1.0f, 1.5f));
        see_enemy_timer_.start ();

        states_ |= Sense::SuspectEnemy;
        aim_flags_ |= AimFlags::LastEnemy;

        enemy_ = new_enemy;
        last_enemy_ = new_enemy;
        last_enemy_origin_ = new_enemy->v.origin;

        return true;
      };

      if (!thru_wall_hold_timer_.elapsed ()) {
        return engage_thru_wall ();
      }

      if (thru_wall_reroll_timer_.elapsed ()) {
        thru_wall_reroll_timer_.start (rg (0.75f, 1.5f));

        if (GetThruWallChance (difficulty_data_->seen_thru_pct)) {
          return engage_thru_wall ();
        }
      }
    }
  }

  if ((aim_flags_ <= AimFlags::PredictPath && see_enemy_timer_.greater_than (3.0f) && game.IsNullEntity (last_enemy_) &&
        game.IsNullEntity (enemy_) && GetTaskId () != TaskId::ShootBreakable && GetTaskId () != TaskId::PlantBomb &&
        GetTaskId () != TaskId::DefuseBomb) ||
      game_state.IsRoundOver ()) {

    if (reload_data_.state == Reload::None) {
      reload_data_.state = Reload::Primary;
    }
  }

  if ((UsesSniper () || UsesZoomableRifle ()) && zoom_check_timer_.remaining_time () < -1.0f) {
    if (pev->fov < 90.0f) {
      pev->button |= IN_ATTACK2;
    }
    else {
      zoom_check_timer_.invalidate ();
    }
  }
  return false;
}

ystl::Vector Bot::GetBodyOffsetError (edict_t *target, float distance) {
  if (game.IsNullEntity (target)) {
    return nullptr;
  }

  if (aim_error_timer_.elapsed ()) {
    const float hit_error = distance / (ystl::max (0.5f + static_cast<float> (difficulty_), 1.0f) * 1280.0f);
    const auto &maxs = target->v.maxs, &mins = target->v.mins;

    aim_last_error_ = ystl::Vector (rg (mins.x * hit_error, maxs.x * hit_error), rg (mins.y * hit_error, maxs.y * hit_error),
      rg (mins.z * hit_error * 0.5f, maxs.z * hit_error * 0.5f));

    const auto &aim_error = difficulty_data_->aim_error;
    aim_last_error_ += ystl::Vector (rg (-aim_error.x, aim_error.x), rg (-aim_error.y, aim_error.y), rg (-aim_error.z, aim_error.z));

    aim_error_timer_.start (rg (0.4f, 0.8f));
  }
  return aim_last_error_;
}

ystl::Vector Bot::GetEnemyBodyOffset () {
  if (!enemy_parts_) {
    if (enemy_origin_.empty () && !last_enemy_origin_.empty () && !game.IsNullEntity (last_enemy_) && has_flag (states_, Sense::SuspectEnemy)) {
      return last_enemy_origin_ + GetBodyOffsetError (last_enemy_, last_enemy_origin_.distance (pev->origin));
    }
    return enemy_origin_.empty () ? last_enemy_origin_ : enemy_origin_;
  }
  const float distance = enemy_->v.origin.distance (pev->origin);

  auto vis_parts = enemy_parts_;

  if (has_flag (vis_parts, Visibility::Body) && !UsesSniper () && distance > 1000.0f + static_cast<float> (Skill ()) * 10.0f) {
    vis_parts &= ~Visibility::Head;
  }
  else if (distance < 800.0f && (current_weapon_ != Weapon::Scout) && UsesSniper ()) {
    vis_parts &= ~Visibility::Head;
  }

  ystl::Vector spot = enemy_->v.origin;
  ystl::Vector compensation = nullptr;

  if (!UsesSniper () && !UsesKnife () && distance > kSprayDistance) {
    compensation = (enemy_->v.velocity - pev->velocity) * frame_interval_ * 1.8f;
    compensation.z = 0.0f;
  }

  const auto head_origin = [&] (edict_t *e) -> ystl::Vector {
    return ystl::Vector { e->v.origin.x, e->v.origin.y, e->v.absmin.z + e->v.size.z * 0.81f };
  };

  if (game.IsPlayerEntity (enemy_)) {
    if (has_flag (vis_parts, Visibility::Head) && has_flag (vis_parts, Visibility::Body)) {
      auto headshot_pct = difficulty_data_->headshot_pct;

      const bool should_pause = NeedToPauseFiring (distance);
      const bool recoil_high = IsRecoilHigh ();

      if (recoil_high || should_pause || (UsesShotgun () && distance > kSprayDistance)) {
        headshot_pct = distance > kSprayDistance ? 6 : ystl::min (headshot_pct, 20);
      }

      if (enemy_body_part_set_ != enemy_) {
        if (head_roll_timer_.elapsed ()) {
          head_roll_timer_.start (rg (0.5f, 1.0f));
          enemy_body_part_set_ = rg.chance (headshot_pct) ? enemy_ : nullptr;
        }
      }
      else if (recoil_high || should_pause) {
        enemy_body_part_set_ = nullptr;
      }

      if (enemy_body_part_set_ == enemy_) {
        spot = head_origin (enemy_);

        if (UsesSniper ()) {
          spot.z -= pev->view_ofs.z * 0.15f;
        }
      }
      else {
        spot = enemy_->v.origin;

        if (difficulty_ == Difficulty::Expert) {
          spot.z += enemy_->v.size.z * 0.1f;
        }
      }
    }
    else if (has_flag (vis_parts, Visibility::Body)) {
      spot = enemy_->v.origin;
    }
    else if (has_flag (vis_parts, Visibility::Other)) {
      spot = enemy_origin_;
    }
    else if (has_flag (vis_parts, Visibility::Head)) {
      spot = head_origin (enemy_);
    }
  }
  spot += compensation;

  if (UsesKnife () && difficulty_ >= Difficulty::Normal) {
    spot = enemy_origin_;
  }

  if (game.IsPlayerEntity (enemy_)) {
    spot += GetAimBias (distance);
  }

  if (!UsesSniper () && !UsesKnife () && distance > kSprayDistance) {
    const float target = ystl::tanf (ystl::deg2rad (pev->punchangle.x)) * distance;
    recoil_compensation_ += (target - recoil_compensation_) * ystl::min (1.0f, frame_interval_ * 10.0f);

    spot.z -= recoil_compensation_;
  }
  else {
    recoil_compensation_ = 0.0f;
  }

  last_enemy_origin_ = spot;

  if (difficulty_ < Difficulty::Normal) {
    spot += GetBodyOffsetError (enemy_, distance);
  }
  return spot;
}

ystl::Vector Bot::GetAimBias (float distance) const {
  AimProfile profile {};
  profile.weapon = weapon_type_;
  profile.distance = distance;
  profile.stance = IsDucking () ? AimStance::Crouching : (pev->velocity.length2d () > 50.0f ? AimStance::Moving : AimStance::Standing);
  profile.exposure = IsDucking () ? AimExposure::Hidden : (has_flag (aim_flags_, AimFlags::Camp) ? AimExposure::Peek : AimExposure::Open);

  const float skill = static_cast<float> (Skill ()) / 100.0f;
  return aim_layer_.Compute (profile) * (1.0f - 0.5f * skill);
}

bool Bot::IsFriendInLineOfFire (float distance) const {
  if (!mp_friendlyfire || game.Is (GameFlags::CSDM)) {
    return false;
  }

  const ystl::Vector eye_pos = GetEyesPos ();
  const ystl::Vector forward = pev->v_angle.forward ();
  const ystl::Vector trace_end = eye_pos + forward * distance;

  const float distance_sq = ystl::sqrf (distance + 256.0f);
  constexpr float kFriendlyFireConeDot = 0.95f;

  for (const auto &client : clients) {
    if (!client.IsTeammate (team_, Ent ())) {
      continue;
    }

    const ystl::Vector &friend_origin = client.ent->v.origin;
    const float friend_dist_sq = friend_origin.distance_sq (pev->origin);

    if (friend_dist_sq <= distance_sq && util.ViewDot (Ent (), friend_origin) >= kFriendlyFireConeDot) {
      return true;
    }
  }

  Trace::Result tr {};
  trace.Line (eye_pos, trace_end, TraceIgnore::None, Ent (), &tr);

  if (game.IsPlayerEntity (tr.hit) && tr.hit != Ent ()) {
    if (game.GetPlayerTeam (tr.hit) == team_ && game.IsAliveEntity (tr.hit)) {
      return true;
    }
  }
  return false;
}

void Bot::FocusEnemy () {
  if (game.IsNullEntity (enemy_)) {
    return;
  }

  look_at_ = GetEnemyBodyOffset ();

  if (!enemy_surprise_timer_.elapsed ()) {
    return;
  }
  const float distance_sq = look_at_.distance_sq2d (GetEyesPos ());

  const float dot = util.ViewDot (Ent (), enemy_origin_);
  const float enemy_dot = util.ViewDot (enemy_, pev->origin);

  if (distance_sq < ystl::sqrf (128.0f) && !UsesSniper ()) {
    if (UsesKnife ()) {
      if (distance_sq < ystl::sqrf (80.0f)) {
        wants_to_fire_ = true;
      }
      else {
        wants_to_fire_ = false;
      }
    }
    else if (dot > 0.80f) {
      wants_to_fire_ = true;
    }
    else {
      wants_to_fire_ = false;
    }
  }
  else {
    if (dot < 0.90f) {
      wants_to_fire_ = false;
    }
    else {
      if (UsesKnife ()) {
        wants_to_fire_ = true;
      }
      else {
        if (enemy_dot >= 0.90f) {
          wants_to_fire_ = true;
        }
        else {
          if (dot > 0.99f) {
            wants_to_fire_ = true;
          }
          else {
            wants_to_fire_ = false;
          }
        }
      }
    }

    if (distance_sq < ystl::sqrf (90.0f)) {
      wants_to_fire_ = true;
    }
  }
}

void Bot::AttackMovement () {
  if (game.IsNullEntity (enemy_)) {
    return;
  }

  if (UsesKnife () || is_creature_) {
    dest_origin_ = enemy_->v.origin;
  }

  if (last_used_nodes_timer_.elapsed_time () < -frame_interval_) {
    return;
  }

  const auto distance_sq = pev->origin.distance_sq (enemy_->v.origin);

  auto approach = 0;

  if (UsesKnife () || is_creature_) {
    approach = 100;
  }
  else if (has_flag (states_, Sense::SuspectEnemy) && !has_flag (states_, Sense::SeeingEnemy)) {
    approach = 49;
  }
  else if (reload_data_.is_reloading || is_vip_ || infected_enemy_team_) {
    approach = 29;
  }
  else {
    approach = static_cast<int> (health_value_ * agression_level_);

    if (UsesSniper () && approach > 49) {
      approach = 49;
    }
  }
  const bool is_enemy_cone = IsInViewCone (enemy_->v.origin);

  if (!UsesKnife () && !is_creature_ && !game.Is (GameFlags::CSDM) && !IsKnifeMode ()) {
    if (has_flag (states_, Sense::SeeingEnemy) && approach < 30 && !game_state.IsBombPlanted () && cover_search_timer_.elapsed () &&
        (is_enemy_cone || is_vip_ || reload_data_.is_reloading || infected_enemy_team_)) {

      StartTask (TaskId::SeekCover, TaskPri::kSeekCover, kInvalidNodeIndex, 0.0f, true);

      if (!CheckWallOnBehind ()) {
        move_speed_ = -pev->maxspeed;
      }
    }
    else if (approach < 50) {
      move_speed_ = 0.0f;
    }
    else {
      move_speed_ = pev->maxspeed;
    }
  }
  else if (UsesKnife () || is_creature_) {
    move_speed_ = pev->maxspeed;
  }

  if (fight_style_check_timer_.elapsed ()) {
    if (UsesKnife () || is_creature_) {
      fight_style_ = Fight::Strafe;
    }
    else if (UsesSniper () && !reload_data_.is_reloading && !sniper_stop_timer_.elapsed ()) {
      fight_style_ = Fight::Stay;
    }
    else if (UsesRifle () || UsesSubmachine () || UsesHeavy ()) {
      const auto rand = rg (1, 100);
      const bool enemy_weapon_is_sniper = has_flag (enemy_->v.weapons, kSniperWeaponMask);
      const auto group_of_enemies = distance_sq < ystl::sqrf (768.0f) && IsGroupOfEnemies (enemy_->v.origin, 384.0f);

      if (distance_sq < ystl::sqrf (768.0f)) {
        fight_style_ = Fight::Strafe;
      }
      else if (distance_sq < ystl::sqrf (1024.0f)) {
        if (group_of_enemies || (enemy_weapon_is_sniper && is_enemy_cone)) {
          fight_style_ = Fight::Strafe;
        }
        else if (rand < (UsesSubmachine () ? 50 : 30)) {
          fight_style_ = Fight::Strafe;
        }
        else {
          fight_style_ = Fight::Stay;
        }
      }
      else {
        if (group_of_enemies || (enemy_weapon_is_sniper && is_enemy_cone)) {
          fight_style_ = Fight::Strafe;
        }
        else if (rand < (UsesSubmachine () ? 80 : 90)) {
          fight_style_ = Fight::Stay;
        }
        else {
          fight_style_ = Fight::Strafe;
        }
      }
    }
    else {
      fight_style_ = Fight::Stay;
    }

    const auto pistol_strafe_distance = game.Is (GameFlags::CSDM) ? kSprayDistanceX2 * 3.0f : kSprayDistanceX2;

    if (approach < 30 || fire_hurts_friend_ ||
        ((UsesPistol () || UsesShotgun ()) && distance_sq < ystl::sqrf (pistol_strafe_distance) && is_enemy_cone)) {
      fight_style_ = Fight::Strafe;
    }
    fight_style_check_timer_.start (rg (1.0f, 3.0f));
  }

  // Человек пятится назад, если враг с ножом/лапами подошел ближе 400 юнитов
  if (!UsesKnife () && !is_creature_ && distance_sq < ystl::sqrf (400.0f)) {
    move_speed_ = -pev->maxspeed;
  }

  if ((UsesKnife () && is_enemy_cone) || is_creature_) {
    if (distance_sq < ystl::sqrf (100.0f)) {
      fight_style_ = Fight::Strafe;
    }
  }

  if (fight_style_ == Fight::Strafe) {
    auto swap_dodge_direction = [&] () {
      dodge_strafe_dir_ = (dodge_strafe_dir_ == Dodge::Left ? Dodge::Right : Dodge::Left);
    };

    auto commit_strafe = [&] () {
      combat_stuck_check_timer_.start ();
      last_combat_strafe_origin_ = pev->origin;
    };

    auto strafe_update_delay = [] () {
      return ystl::rg (0.3f, 0.8f);
    };

    if (strafe_set_timer_.elapsed ()) {
      const ystl::Vector dir_to_point = (pev->origin - enemy_->v.origin).normalize2d ();
      const ystl::Vector right_side = enemy_->v.v_angle.right ().normalize2d ();

      if ((dir_to_point | right_side) < 0.0f) {
        dodge_strafe_dir_ = Dodge::Right;
      }
      else {
        dodge_strafe_dir_ = Dodge::Left;
      }

      if (rg.chance (30)) {
        swap_dodge_direction ();
      }
      strafe_set_timer_.start (strafe_update_delay ());
      commit_strafe ();
    }
    const float wall_check_dist = ystl::min (134.0f, ystl::sqrtf (distance_sq) * 0.25f);

    const bool wall_on_right = CheckWallOnRight (wall_check_dist);
    const bool wall_on_left = CheckWallOnLeft (wall_check_dist);

    if (dodge_strafe_dir_ == Dodge::Left) {
      if (!wall_on_left) {
        strafe_speed_ = -pev->maxspeed;
      }
      else if (!wall_on_right) {
        swap_dodge_direction ();

        strafe_set_timer_.start (strafe_update_delay ());
        commit_strafe ();
        strafe_speed_ = pev->maxspeed;
      }
      else {
        strafe_speed_ = 0.0f;
        strafe_set_timer_.start (strafe_update_delay ());
        commit_strafe ();
      }
    }
    else {
      if (!wall_on_right) {
        strafe_speed_ = pev->maxspeed;
      }
      else if (!wall_on_left) {
        swap_dodge_direction ();

        strafe_set_timer_.start (strafe_update_delay ());
        commit_strafe ();
        strafe_speed_ = -pev->maxspeed;
      }
      else {
        strafe_speed_ = 0.0f;
        strafe_set_timer_.start (strafe_update_delay ());
        commit_strafe ();
      }
    }

    if (combat_stuck_check_timer_.greater_than (0.25f) && !ystl::fzero (strafe_speed_)) {
      const float strafe_moved = last_combat_strafe_origin_.distance_sq2d (pev->origin);

      if (strafe_moved < 4.0f && pev->velocity.length2d () > 0.0f) {
        swap_dodge_direction ();
        commit_strafe ();
        strafe_set_timer_.start (strafe_update_delay ());
      }
    }

    if (wall_on_right && wall_on_left && !UsesKnife ()) {
      strafe_speed_ = 0.0f;
      move_speed_ = 0.0f;

      strafe_set_timer_.start (3.0f);
      dodge_strafe_dir_ = Dodge::None;
    }

    if (!UsesKnife () && approach >= 30) {
      move_speed_ = 0.0f;
    }

    if (difficulty_ >= Difficulty::Normal && distance_sq < ystl::sqrf (kSprayDistance) && (reload_data_.is_reloading || health_value_ < 35.0f) &&
        jump_time_ + 5.0f < game.Time () && IsOnFloor () && pev->velocity.length2d () > 150.0f && !UsesSniper () && is_enemy_cone) {

      pev->button |= IN_JUMP;
    }
  }
  else if (fight_style_ == Fight::Stay) {
    if ((UsesKnife () || is_creature_) && distance_sq < ystl::sqrf (128.0f)) {
      move_speed_ = pev->maxspeed;
      strafe_speed_ = 0.0f;
    }
    else {
      const bool already_ducking = !duck_timer_.elapsed () || IsDucking () || ((pev->button | pev->oldbuttons) & IN_DUCK);

      if (already_ducking && IsInNarrowPlace ()) {
        duck_timer_.start (frame_interval_ * 3.0f);
      }
      move_speed_ = 0.0f;
      strafe_speed_ = 0.0f;
    }
  }

  if (reload_data_.is_reloading) {
    move_speed_ = -pev->maxspeed;

    if (is_enemy_cone) {
      duck_timer_.invalidate ();
    }
  }

  if (!IsInWater () && !IsOnLadder () && (!ystl::fzero (move_speed_) || !ystl::fzero (strafe_speed_))) {
    ystl::Vector right {}, forward {};
    pev->v_angle.angle_vectors (&forward, &right, nullptr);

    const ystl::Vector front = forward * move_speed_ * 0.2f;
    const ystl::Vector side = right * strafe_speed_ * 0.2f;
    const ystl::Vector spot = pev->origin + front + side + pev->velocity * frame_interval_;

    if (IsNotSafeToMove (spot)) {
      const ystl::Vector strafe_fix = pev->origin + front + right * -strafe_speed_ * 0.2f + pev->velocity * frame_interval_;

      if (!IsNotSafeToMove (strafe_fix)) {
        strafe_speed_ = -strafe_speed_;
      }
      else {
        strafe_speed_ = -strafe_speed_;
        move_speed_ = -move_speed_;
      }
      pev->button &= ~IN_JUMP;
    }
  }
  IgnoreCollision ();
}

bool Bot::IsKnifeMode () {
  return cv_jasonmode || (UsesKnife () && !HasAnyWeapons ()) || is_creature_ ||
         (has_flag (states_, Sense::SeeingEnemy) && UsesKnife () && !HasAnyAmmoInClip ());
}

bool Bot::IsGrenadeWar () {
  const bool has_some_greandes = GetBestGrenadeCarriedId () != Weapon::Invalid;

  if (!HasAnyWeapons () && has_some_greandes) {
    return true;
  }

  if (cv_grenadier_mode) {
    return true;
  }
  return game.MapIs (MapFlags::GrenadeWar);
}

void Bot::UpdateTeamCommands () {
  if (team_order_timer_.remaining_time () > 2.0f || game.Is (GameFlags::FreeForAll) || !cv_radio_mode.As<int> ()) {
    return;
  }

  bool member_near = false;
  bool member_exists = false;

  for (const auto &client : clients) {
    if (!client.IsTeammate (team_, Ent ())) {
      continue;
    }
    member_exists = true;

    if (SeesEntity (client.origin)) {
      member_near = true;
      break;
    }
  }

  if (member_near) {
    if (personality_ == Personality::Rusher && cv_radio_mode.As<int> () == 2) {
      PushRadioChat (RadioChat::StormTheFront);
    }
    else if (personality_ != Personality::Rusher && cv_radio_mode.As<int> () == 2) {
      PushRadioChat (RadioChat::TeamFallback);
    }
  }
  else if (member_exists && cv_radio_mode.As<int> () == 1) {
    PushRadioChat (RadioChat::TakingFireNeedAssistance);
  }
  else if (member_exists && cv_radio_mode.As<int> () == 2) {
    PushRadioChat (RadioChat::ScaredEmotion);
  }
  team_order_timer_.start (rg (15.0f, 30.0f));
}

bool Bot::IsGroupOfEnemies (const ystl::Vector &location, float radius) {
  int num_players = 0;
  const float radius_sq = ystl::sqrf (radius);

  for (const auto &client : clients) {
    if (!client.IsUsedAndAlive () || client.IsSameTeam (team_)) {
      continue;
    }

    if (client.IsInRadius (location, radius_sq)) {
      if (!SeesEntity (client.origin)) {
        continue;
      }
      ++num_players;
    }
  }

  if (num_players < 2) {
    return false;
  }
  return true;
}

float Bot::CalculateScaleFactor (edict_t *ent) const {
  const ystl::Vector ent_size = ent->v.maxs - ent->v.mins;
  const float ent_area = 2.0f * (ent_size.x * ent_size.y + ent_size.y * ent_size.z + ent_size.x * ent_size.z);

  const ystl::Vector bot_size = pev->maxs - pev->mins;
  const float bot_area = 2.0f * (bot_size.x * bot_size.y + bot_size.y * bot_size.z + bot_size.x * bot_size.z);

  return ent_area / bot_area;
}

ystl::Vector Bot::CalcToss (const ystl::Vector &start, const ystl::Vector &stop) {
  static constexpr float kGravityFactor = 0.55f;
  static constexpr float kMinAscentTime = 0.1f;
  static constexpr float kMaxHeightDiff = 500.0f;
  static constexpr float kCeilingTraceHeight = 500.0f;
  static constexpr float kTargetHeightOffset = 15.0f;
  static constexpr float kVelocityScale = 0.777f;
  static constexpr float kMaxWallDot = 0.75f;
  static constexpr float kMinTraceFraction = 0.8f;

  static constexpr float kArcSpeed = 195.0f;
  static constexpr float kArcMaxFlightTime = 2.0f;
  static constexpr float kArcClampedFlightTime = 1.2f;

  const float gravity = sv_gravity.As<float> () * kGravityFactor;

  if (ystl::fzero (gravity)) {
    return nullptr;
  }

  ystl::Vector target = stop;
  target.z -= kTargetHeightOffset;

  if (ystl::abs (target.z - start.z) > kMaxHeightDiff) {
    return nullptr;
  }

  ystl::Vector mid_point = start + (target - start) * 0.5f;
  Trace::Result tr {};

  trace.Hull (mid_point, mid_point + ystl::Vector (0.0f, 0.0f, kCeilingTraceHeight), TraceIgnore::Monsters, head_hull, Ent (), &tr);

  if (tr.fraction < 1.0f && tr.hit) {
    mid_point = tr.end_pos;
    mid_point.z = tr.hit->v.absmin.z - 1.0f;
  }
  else {
    float flight_time = (target - start).length () / kArcSpeed;

    if (flight_time > kArcMaxFlightTime) {
      flight_time = kArcClampedFlightTime;
    }
    const float half_time = flight_time * 0.5f;
    mid_point.z += 0.5f * gravity * half_time * half_time;
  }

  if (mid_point.z < start.z || mid_point.z < target.z) {
    return nullptr;
  }

  const float ascent_height = mid_point.z - start.z;
  const float time_to_apex = ystl::sqrtf (ascent_height / (0.5f * gravity));

  if (time_to_apex < kMinAscentTime) {
    return nullptr;
  }

  const float descent_height = mid_point.z - target.z;
  const float time_from_apex = ystl::sqrtf (descent_height / (0.5f * gravity));

  const float total_time = time_to_apex + time_from_apex;

  ystl::Vector velocity = (target - start) / total_time;
  velocity.z = gravity * time_to_apex;

  ystl::Vector apex = start + velocity * time_to_apex;
  apex.z = mid_point.z;

  trace.Hull (start, apex, TraceIgnore::None, head_hull, Ent (), &tr);

  if (tr.fraction < 1.0f || tr.all_solid) {
    return nullptr;
  }

  trace.Hull (target, apex, TraceIgnore::Monsters, head_hull, Ent (), &tr);

  if (!ystl::fequal (tr.fraction, 1.0f)) {
    const float dot = tr.plane_normal | (apex - target).normalize ();

    if (dot > kMaxWallDot || tr.fraction < kMinTraceFraction) {
      return nullptr;
    }
  }
  return velocity * kVelocityScale;
}

ystl::Vector Bot::CalcThrow (const ystl::Vector &start, const ystl::Vector &stop) {
  static constexpr float kGravityFactor = 0.55f;
  static constexpr float kThrowSpeed = 195.0f;
  static constexpr float kMinFlightTime = 0.01f;
  static constexpr float kMaxFlightTime = 2.0f;
  static constexpr float kClampedFlightTime = 1.2f;
  static constexpr float kVelocityScale = 0.7793f;
  static constexpr float kMaxWallDot = 0.75f;
  static constexpr float kMinTraceFraction = 0.8f;

  const float gravity = sv_gravity.As<float> () * kGravityFactor;

  if (ystl::fzero (gravity)) {
    return nullptr;
  }

  ystl::Vector displacement = stop - start;
  float flight_time = displacement.length () / kThrowSpeed;

  if (flight_time < kMinFlightTime) {
    return nullptr;
  }

  if (flight_time > kMaxFlightTime) {
    flight_time = kClampedFlightTime;
  }

  const float half_time = flight_time * 0.5f;

  ystl::Vector velocity = displacement * (1.0f / flight_time);
  velocity.z += gravity * half_time;

  ystl::Vector apex = start + displacement * 0.5f;
  apex.z += 0.5f * gravity * half_time * half_time;

  Trace::Result tr {};

  trace.Hull (start, apex, TraceIgnore::None, head_hull, Ent (), &tr);

  if (!ystl::fequal (tr.fraction, 1.0f)) {
    return nullptr;
  }

  trace.Hull (stop, apex, TraceIgnore::Monsters, head_hull, Ent (), &tr);

  if ((!ystl::fequal (tr.fraction, 1.0f) || tr.all_solid)) {
    const float dot = tr.plane_normal | (apex - stop).normalize ();

    if (dot > kMaxWallDot || tr.fraction < kMinTraceFraction) {
      return nullptr;
    }
  }
  return velocity * kVelocityScale;
}

edict_t *Bot::SetCorrectGrenadeVelocity (ystl::StringRef model) {
  edict_t *result = nullptr;

  game.SearchEntities ("classname", "grenade", [&] (edict_t *ent) {
    if (ent->v.owner == this->Ent () && game.IsEntityModelMatches (ent, model)) {
      result = ent;

      if (grenade_.length_sq () > 100.0f) {
        ent->v.velocity = grenade_ + (grenade_ * frame_interval_ * 4.0f);
      }
      grenade_check_timer_.start (3.0f);

      SelectBestWeapon ();
      CompleteTask ();

      return EntitySearchResult::Break;
    }
    return EntitySearchResult::Continue;
  });
  return result;
}

void Bot::CheckGrenadesThrow () {
  const bool preventible_tasks = HasBombTask ();
  const bool is_grenade_mode = IsGrenadeWar ();

  auto clear_throw_states = [] (Sense &states) {
    states &= ~(Sense::ThrowExplosive | Sense::ThrowFlashbang | Sense::ThrowSmoke);
  };

  const auto throwing_condition =
    is_grenade_mode ? last_enemy_origin_.empty ()
                    : (preventible_tasks || IsInNarrowPlace () || cv_ignore_enemies || is_using_grenade_ || reload_data_.is_reloading ||
                        (IsKnifeMode () && !game_state.IsBombPlanted ()) || !grenade_check_timer_.elapsed () || last_enemy_origin_.empty ());

  if (throwing_condition) {
    clear_throw_states (states_);
    return;
  }

  grenade_check_timer_.start (kGrenadeCheckTime);

  const auto sense_condition = is_grenade_mode ? false : !has_flag (states_, Sense::SuspectEnemy | Sense::HearingEnemy);

  if (!game.IsAliveEntity (last_enemy_) || sense_condition) {
    clear_throw_states (states_);
    return;
  }

  const auto grenade_to_throw = GetBestGrenadeCarriedId ();

  if (grenade_to_throw == Weapon::Invalid) {
    grenade_check_timer_.start (15.0f);
    clear_throw_states (states_);
    return;
  }
  else if (!is_grenade_mode) {
    int cancel_prob = agression_level_ > fear_level_ ? 5 : 20;

    if (grenade_to_throw == Weapon::Flashbang) {
      cancel_prob = difficulty_ >= Difficulty::Normal ? 20 : 30;
    }
    else if (grenade_to_throw == Weapon::Smoke) {
      if (in_bomb_zone_ || health_value_ < 50.0f || has_flag (last_enemy_->v.weapons, kSniperWeaponMask)) {
        cancel_prob = difficulty_ >= Difficulty::Normal ? 15 : 25;
      }
      else {
        cancel_prob = difficulty_ >= Difficulty::Normal ? 25 : 35;
      }
    }
    if (rg.chance (cancel_prob)) {
      clear_throw_states (states_);
      return;
    }
  }
  float distance_sq = last_enemy_origin_.distance_sq2d (pev->origin);

  if (!(last_enemy_->v.flags & (FL_ONGROUND | FL_PARTIALGROUND)) && !last_enemy_->v.waterlevel && last_enemy_origin_.z > pev->absmax.z) {
    distance_sq = kInfiniteDistance;
  }

  if (last_enemy_->v.origin.z > pev->origin.z + 500.0f) {
    distance_sq = kInfiniteDistance;
  }

  if (!is_grenade_mode &&
      (has_flag (states_, Sense::SeeingEnemy) && game.IsAliveEntity (enemy_) && ((enemy_->v.button | enemy_->v.oldbuttons) & IN_ATTACK) &&
        util.IsVisible (pev->origin, enemy_)) &&
      util.IsInViewCone (pev->origin, enemy_)) {

    distance_sq = kInfiniteDistance;
  }

  if (!is_grenade_mode && see_enemy_timer_.less_than (kGrenadeCheckTime * 0.2f)) {
    distance_sq = kInfiniteDistance;
  }

  const auto grenade_to_throw_condition = is_grenade_mode                     ? kGrenadeDamageRadius / 4.0f
                                          : grenade_to_throw == Weapon::Smoke ? 200.0f
                                                                              : kGrenadeDamageRadius;

  if (distance_sq > ystl::sqrf (grenade_to_throw_condition) && distance_sq < ystl::sqrf (kGrenadeDamageRadius * 3.0f)) {
    bool allow_throwing = true;

    switch (grenade_to_throw) {
    case Weapon::Explosive:
      if (mp_friendlyfire && NumFriendsNear (last_enemy_->v.origin, 256.0f) > 0) {
        allow_throwing = false;
      }
      else {
        const auto radius = ystl::max (192.0f, last_enemy_->v.velocity.length2d ());
        const ystl::Vector pos = last_enemy_->v.velocity.get2d () + last_enemy_->v.origin;

        auto predicted = graph.GetNearestInRadius (radius, pos, 12);

        if (predicted.empty ()) {
          states_ &= ~Sense::ThrowExplosive;
          break;
        }

        for (const auto &predict : predicted) {
          allow_throwing = true;

          if (!graph.Exists (predict)) {
            allow_throwing = false;
            continue;
          }
          throw_ = graph[predict].origin;

          auto throw_pos = CalcThrow (GetEyesPos (), throw_);

          if (throw_pos.length_sq () < 100.0f) {
            throw_pos = CalcToss (GetEyesPos (), throw_);
          }

          if (throw_pos.empty ()) {
            allow_throwing = false;
          }
          else {
            throw_.z += 110.0f;
            break;
          }
        }
      }

      if (allow_throwing) {
        states_ |= Sense::ThrowExplosive;
      }
      else {
        states_ &= ~Sense::ThrowExplosive;
      }
      break;

    case Weapon::Flashbang: {
      const auto radius = ystl::max (192.0f, last_enemy_->v.velocity.length2d ());
      const ystl::Vector pos = last_enemy_->v.velocity.get2d () + last_enemy_->v.origin;

      auto predicted = graph.GetNearestInRadius (radius, pos, 8);

      if (predicted.empty ()) {
        states_ &= ~Sense::ThrowFlashbang;
        break;
      }

      for (const auto &predict : predicted) {
        allow_throwing = true;

        if (!graph.Exists (predict)) {
          allow_throwing = false;
          continue;
        }
        throw_ = graph[predict].origin;

        if (NumFriendsNear (throw_, 256.0f) > 0) {
          allow_throwing = false;
          continue;
        }
        auto throw_pos = CalcThrow (GetEyesPos (), throw_);

        if (throw_pos.length_sq () < 100.0f) {
          throw_pos = CalcToss (GetEyesPos (), throw_);
        }

        if (throw_pos.empty ()) {
          allow_throwing = false;
        }
        else {
          throw_.z += 110.0f;
          break;
        }
      }

      if (allow_throwing) {
        states_ |= Sense::ThrowFlashbang;
      }
      else {
        states_ &= ~Sense::ThrowFlashbang;
      }
      break;
    }

    case Weapon::Smoke:
      if (allow_throwing && !game.IsNullEntity (last_enemy_)) {
        ystl::Vector smoke_target {};
        const ystl::Vector to_enemy = (last_enemy_->v.origin - pev->origin).normalize ();
        const float distance_to_enemy = last_enemy_->v.origin.distance (pev->origin);

        if (in_bomb_zone_ && (has_c4_ || GetTaskId () == TaskId::PlantBomb)) {
          const float smoke_distance = distance_to_enemy * rg (0.3f, 0.4f);
          smoke_target = pev->origin + to_enemy * smoke_distance;
        }
        else if (health_value_ < 50.0f || reload_data_.is_reloading) {
          const float smoke_distance = distance_to_enemy * rg (0.25f, 0.35f);
          smoke_target = pev->origin + to_enemy * smoke_distance;
        }
        else if (has_flag (last_enemy_->v.weapons, kSniperWeaponMask)) {
          const float smoke_distance = distance_to_enemy * rg (0.45f, 0.55f);
          smoke_target = pev->origin + to_enemy * smoke_distance;
        }
        else {
          const float smoke_distance = distance_to_enemy * rg (0.4f, 0.6f);
          smoke_target = pev->origin + to_enemy * smoke_distance;
        }

        const int smoke_node_index = graph.GetNearest (smoke_target);

        if (graph.Exists (smoke_node_index)) {
          throw_ = graph[smoke_node_index].origin;

          auto throw_pos = CalcThrow (GetEyesPos (), throw_);

          if (throw_pos.length_sq () < 100.0f) {
            throw_pos = CalcToss (GetEyesPos (), throw_);
          }

          if (throw_pos.empty ()) {
            allow_throwing = false;
          }
          else {
            throw_.z += 110.0f;
          }
        }
        else {
          allow_throwing = false;
        }
      }
      else {
        allow_throwing = false;
      }

      if (allow_throwing) {
        states_ |= Sense::ThrowSmoke;
      }
      else {
        states_ &= ~Sense::ThrowSmoke;
      }
      break;

    default:
      clear_throw_states (states_);
      return;
    }
    const float max_throw_time = game.Time () + kGrenadeCheckTime * 3.6f;

    if (has_flag (states_, Sense::ThrowExplosive)) {
      StartTask (TaskId::ThrowExplosive, TaskPri::kThrow, kInvalidNodeIndex, max_throw_time, false);
    }
    else if (has_flag (states_, Sense::ThrowFlashbang)) {
      StartTask (TaskId::ThrowFlashbang, TaskPri::kThrow, kInvalidNodeIndex, max_throw_time, false);
    }
    else if (has_flag (states_, Sense::ThrowSmoke)) {
      StartTask (TaskId::ThrowSmoke, TaskPri::kThrow, kInvalidNodeIndex, max_throw_time, false);
    }
  }
  else {
    clear_throw_states (states_);
  }
}

bool Bot::IsEnemyInSight (ystl::Vector &end_pos) {
  Trace::Result aim_hit_tr {};
  trace.Model (GetEyesPos (), GetEyesPos () + pev->v_angle.forward () * kInfiniteDistance, 0, enemy_, &aim_hit_tr);

  if (aim_hit_tr.hit != enemy_) {
    return false;
  }
  end_pos = aim_hit_tr.end_pos;
  return true;
}

bool Bot::IsEnemyNoticeable (float range) {
  if (IsOnLadder ()) {
    return false;
  }
  const float speed_sq = enemy_->v.velocity.length_sq ();

  if (speed_sq > ystl::sqrf (200.0f)) {
    return true;
  }

  struct GaitOdds {
    float near_chance;
    float far_chance;
  };

  static constexpr GaitOdds kOdds[2][2] = {
    { { 100.0f, 10.0f }, { 80.0f, 5.0f }  },
    { { 100.0f, 75.0f }, { 90.0f, 60.0f } },
  };

  const bool walking = speed_sq > ystl::sqrf (30.0f);
  const bool crouching = (enemy_->v.flags & FL_DUCKING) == FL_DUCKING;
  const auto odds = kOdds[walking][crouching];

  const float distance_share = ystl::clamp ((range - 300.0f) / 700.0f, 0.0f, 1.0f);
  float chance = odds.near_chance + (odds.far_chance - odds.near_chance) * distance_share;

  float shown = 0.0f;

  if (has_flag (enemy_parts_, Visibility::Body)) {
    shown += 40.0f;
  }

  if (has_flag (enemy_parts_, Visibility::Head)) {
    shown += 10.0f;
  }

  if (has_flag (enemy_parts_, Visibility::Other)) {
    shown += rg (10.0f, 25.0f);
  }
  chance *= shown / 100.0f;

  chance += 0.5f + 0.5f * static_cast<float> (Skill ());

  if (agression_level_ > fear_level_) {
    chance += 50.0f;
  }
  chance = ystl::max (0.1f, chance * ystl::abs (agression_level_ - fear_level_));

  return rg (0.0f, 100.0f) < chance;
}

bool Bot::IsEnemyThreat () {
  if (game.IsNullEntity (enemy_) || has_flag (states_, Sense::SuspectEnemy) || GetTaskId () == TaskId::SeekCover) {
    return false;
  }

  if (GetTaskId () == TaskId::Camp) {
    return false;
  }

  auto is_on_attack_distance = [&] (edict_t *e, const float distance) -> bool {
    const float distance_sq = e->v.origin.distance_sq (pev->origin);

    if (distance_sq < ystl::sqrf (distance)) {
      return true;
    }

    if (UsesKnife () && distance_sq < ystl::sqrf (distance)) {
      return true;
    }
    return false;
  };

  if (is_on_attack_distance (enemy_, 256.0f) || (!UsesKnife () && IsInViewCone (enemy_->v.origin))) {
    return true;
  }
  return false;
}

bool Bot::ReactOnEnemy () {
  if (!IsEnemyThreat ()) {
    return false;
  }

  if (game.IsNullEntity (enemy_)) {
    is_enemy_reachable_ = false;
    return false;
  }

  if (is_creature_) {
    is_enemy_reachable_ = pev->origin.distance_sq2d (enemy_->v.origin) < ystl::sqrf (128.0f);

    if (is_enemy_reachable_) {
      nav_timer_.start ();
    }
    return is_enemy_reachable_;
  }

  if (enemy_reachable_timer_.elapsed ()) {
    const auto line_dist = enemy_->v.origin.distance (pev->origin);

    if (IsEnemyNoticeable (line_dist)) {
      is_enemy_reachable_ = true;
    }
    else {
      int own_index = current_node_index_;

      if (own_index == kInvalidNodeIndex) {
        own_index = FindNearestNode ();
      }
      const auto enemy_index = graph.GetNearest (enemy_->v.origin);
      const auto path_dist = planner.PreciseDistance (own_index, enemy_index);

      is_enemy_reachable_ = (path_dist - line_dist <= 112.0f) && !IsOnLadder ();
    }
    enemy_reachable_timer_.start (0.75f);
  }

  if (is_enemy_reachable_) {
    nav_timer_.start ();
    return true;
  }
  return false;
}

bool Bot::GetThruWallChance (int pct) const {
  return rg.chance (pct > 25 ? rg (25, pct) : pct);
}

bool Bot::LastEnemyShootable () {
  if (!has_flag (aim_flags_, AimFlags::LastEnemy) || last_enemy_origin_.empty () || game.IsNullEntity (last_enemy_)) {
    return false;
  }
  return util.ViewDot (Ent (), last_enemy_origin_) >= 0.90f && IsPenetrableObstacleCached (last_enemy_origin_);
}

} // namespace bot
