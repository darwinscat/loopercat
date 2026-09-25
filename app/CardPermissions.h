// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/DeviceProfile.hpp>

#include <string>
#include <string_view>

//==============================================================================
// loopercat::CardPermissions — what this app may ask of the card in front of
// it, read off the profile table by the card's own name for its model
// (DeviceProfile.hpp): the same table the core refuses by, asked one step
// earlier, so that the screen never offers a gesture the core would refuse.
// A pill that cannot flip is not drawn as a button; an editor that cannot
// commit is read-only; a menu of writes says what this app writes to.
//
// A name the table does not know — no card yet, or a card the guard turned
// away — permits nothing. So does no provider at all: a widget that was
// never told what it may do offers nothing, and the RC-5 keeps everything
// because the owner tells it so.
//==============================================================================
namespace loopercat
{

struct CardPermissions {
    bool rename = false;
    bool tempo = false;
    bool oneShot = false;
    bool countIn = false;
    bool push = false;
    bool swap = false;
    bool normalize = false;
    bool clear = false;

    bool anyWrite() const
    {
        return rename || tempo || oneShot || countIn || push || swap || normalize || clear;
    }

    static CardPermissions of(std::string_view family)
    {
        const profile::DeviceProfile* model = profile::findFamily(family);
        if (model == nullptr)
            return {};
        using profile::Operation;
        return { model->allows(Operation::rename),   model->allows(Operation::setTempo),
                 model->allows(Operation::setOneShot), model->allows(Operation::setCountIn),
                 model->allows(Operation::push),     model->allows(Operation::swap),
                 model->allows(Operation::normalize), model->allows(Operation::clear) };
    }

    // The sentence a read-only screen shows, from the table: the models this
    // app has any write open for.
    static std::string writesOnlyTo()
    {
        std::string models;
        for (const profile::DeviceProfile* candidate : profile::kAll)
            if (candidate->allowsWrites())
                models += (models.empty() ? "" : ", ") + std::string(candidate->familyName);
        return "LooperCat writes only to the " + models;
    }
};

} // namespace loopercat
