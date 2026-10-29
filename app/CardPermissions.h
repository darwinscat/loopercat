// Copyright (c) 2026 Darwin's Cat. Part of Looper Cat — see LICENSE.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <loopercat/DeviceProfile.hpp>

#include <string>
#include <string_view>
#include <vector>

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
    bool trim = false;
    bool rhythm = false;
    bool playStop = false;
    bool pull = false;
    bool restore = false;
    bool downmix = false;
    // The one short sentence a closed control says on a card whose model has
    // some operations open and others not ("On the RC-500, LooperCat does
    // rename, set one-shot, pull, clear and restore only"), or, on a model this app only
    // reads, what it writes to. Empty when every operation is open, and when
    // no card the table knows is in front of the app.
    std::string closed;

    bool anyWrite() const
    {
        return rename || tempo || oneShot || countIn || push || swap || normalize || clear
            || trim || rhythm || playStop || restore || downmix;
    }

    static CardPermissions of(std::string_view family)
    {
        const profile::DeviceProfile* model = profile::findFamily(family);
        if (model == nullptr)
            return {};
        using profile::Operation;
        CardPermissions can;
        can.rename = model->allows(Operation::rename);
        can.tempo = model->allows(Operation::setTempo);
        can.oneShot = model->allows(Operation::setOneShot);
        can.countIn = model->allows(Operation::setCountIn);
        can.push = model->allows(Operation::push);
        can.swap = model->allows(Operation::swap);
        can.normalize = model->allows(Operation::normalize);
        can.clear = model->allows(Operation::clear);
        can.trim = model->allows(Operation::trim);
        can.rhythm = model->allows(Operation::setRhythm);
        can.playStop = model->allows(Operation::setPlayStop);
        can.pull = model->allows(Operation::pull);
        can.restore = model->allows(Operation::restore);
        can.downmix = model->allows(Operation::downmix);
        if (!model->allowsWrites())
            can.closed = writesOnlyTo();
        else if (model->operations != profile::kEveryOperation)
            can.closed = "On the " + std::string(model->familyName) + ", LooperCat does "
                       + profile::openOperations(*model) + " only";
        return can;
    }

    // The sentence a read-only screen shows, from the table: the models this
    // app has any write open for.
    static std::string writesOnlyTo()
    {
        std::vector<std::string> models;
        for (const profile::DeviceProfile* candidate : profile::kAll)
            if (candidate->allowsWrites())
                models.push_back("the " + std::string(candidate->familyName));
        std::string list;
        for (std::size_t i = 0; i < models.size(); ++i)
            list += (i == 0 ? "" : i + 1 == models.size() ? " and " : ", ") + models[i];
        return "LooperCat writes only to " + list;
    }

    // What a pane that edits a memory says under its controls: that a change
    // is heard after a disconnect, when it can take one, and what this card
    // keeps closed, when it keeps something.
    std::string footer(bool paneCanWrite) const
    {
        if (!paneCanWrite)
            return closed.empty() ? writesOnlyTo() : closed;
        const std::string heard = "Disconnect to hear the changes.";
        return closed.empty() ? heard : heard + " " + closed + ".";
    }
};

} // namespace loopercat
