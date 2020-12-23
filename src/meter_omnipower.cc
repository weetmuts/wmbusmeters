/*
 Copyright (C) 2018-2019 Fredrik Öhrström

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
Implemented October 2020 Janus Bo Andersen:
Implements Kamstrup OmniPower, energy meter.
This C1 WM-Bus meter broadcasts:
- Accumulated energy consumption (A+, kWh)
- Accumulated energy production (A-, kWh)
- Current power consumption (P+, kW)
- Current power production (P-, kW)

According to Kamstrup doc. 58101496_C1_GB_05.2018
(Wireless M-Bus Module for OMNIPOWER), the single-phase,
three-phase and CT meters send the same datagram.

Meter version. Implementation tested against meter:
Kamstrup one-phase with firmware version 0x30.

Encryption:
Meter uses AES-128 in CTR mode, which is the only mode supported by
the extended link layer (wm-bus), see EN 13757-4:2019.
Security mode is set during instatiation as 
TPLSecurityMode::AES_CBC_IV, but this is overridden anyway when 
reading the 3 ENC bits using the function in the wmbus.cc file.

*/

#include"dvparser.h"
#include"meters.h"
#include"meters_common_implementation.h"
#include"wmbus.h"
#include"wmbus_utils.h"
#include"util.h"
#include <iostream>

struct MeterOmnipower : public virtual ElectricityMeter, public virtual MeterCommonImplementation {
    MeterOmnipower(WMBus *bus, MeterInfo &mi);

    double totalEnergyConsumption(Unit u);
    double totalEnergyBackward(Unit u);
    double powerConsumption(Unit u);
    double powerBackward(Unit u);

private:

    void processContent(Telegram *t);

    double total_energy_kwh_{};
    double total_energy_backward_kwh_{};
    double power_kw_{};
    double power_backward_kw_{};

};

unique_ptr<ElectricityMeter> createOmnipower(WMBus *bus, MeterInfo &mi)
{
    return unique_ptr<ElectricityMeter>(new MeterOmnipower(bus, mi));
}

MeterOmnipower::MeterOmnipower(WMBus *bus, MeterInfo &mi) :
    MeterCommonImplementation(bus, mi, MeterType::OMNIPOWER)
{
    setExpectedTPLSecurityMode(TPLSecurityMode::AES_CBC_IV);

    addLinkMode(LinkMode::C1);

    addPrint("total_energy_consumption", Quantity::Energy,
             [&](Unit u){ return totalEnergyConsumption(u); },
             "The total energy consumption recorded by this meter.",
             true, true);

    addPrint("total_energy_backward", Quantity::Energy,
             [&](Unit u){ return totalEnergyBackward(u); },
             "The total energy backward recorded by this meter.",
             true, true);

    addPrint("power_consumption", Quantity::Power,
             [&](Unit u){ return powerConsumption(u); },
             "The current power consumption on this meter.",
             true, true);

    addPrint("power_backward", Quantity::Power,
             [&](Unit u){ return powerBackward(u); },
             "The current power backward on this meter.",
             true, true);
}

double MeterOmnipower::totalEnergyConsumption(Unit u)
{
    assertQuantity(u, Quantity::Energy);
    return convert(total_energy_kwh_, Unit::KWH, u);
}

double MeterOmnipower::totalEnergyBackward(Unit u)
{
    assertQuantity(u, Quantity::Energy);
    return convert(total_energy_backward_kwh_, Unit::KWH, u);
}

double MeterOmnipower::powerConsumption(Unit u)
{
    assertQuantity(u, Quantity::Power);
    return convert(power_kw_, Unit::KW, u);
}

double MeterOmnipower::powerBackward(Unit u)
{
    assertQuantity(u, Quantity::Power);
    return convert(power_backward_kw_, Unit::KW, u);
}


void MeterOmnipower::processContent(Telegram *t)
{

    // Data Record header established from telegram analysis
    // 04 04 (32 bit uint) Energy 10^1 Wh (consumption), A+
    // 04 84 3C (32 bit uint) Energy 10^1 Wh (production), A-
    // 04 2B (32 bit uint) Power 10^0 W (consumption), P+
    // 04 AB 3C (32 bit uint) Power 10^0 W (production), P-

    int offset;
    extractDVdouble(&t->values, "0404", &offset, &total_energy_kwh_);
    t->addMoreExplanation(offset, " total energy (%f kwh)", total_energy_kwh_);

    extractDVdouble(&t->values, "04843C", &offset, &total_energy_backward_kwh_);
    t->addMoreExplanation(offset, " total energy backward (%f kwh)", total_energy_backward_kwh_);

    extractDVdouble(&t->values, "042B", &offset, &power_kw_);
    t->addMoreExplanation(offset, " current power (%f kw)", power_kw_);

    extractDVdouble(&t->values, "04AB3C", &offset, &power_backward_kw_);
    t->addMoreExplanation(offset, " current power (%f kw)", power_backward_kw_);

}

