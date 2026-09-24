// wM-Buster ADV — core wM-Bus types: quantities, units, VIF ranges, VIF
// combinables and measurement types. Tables mirror wmbusmeters (units.h,
// dvparser.h) so that driver definitions generated from the upstream XMQ
// sources are interpreted with identical semantics.
// GPL-3.0
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace wmb {

// ---------------------------------------------------------------------------
// Quantities
// ---------------------------------------------------------------------------
#define WMB_LIST_OF_QUANTITIES \
    X(Time) X(Length) X(Mass) X(Amperage) X(Temperature) X(AmountOfSubstance) \
    X(LuminousIntensity) X(Energy) X(Reactive_Energy) X(Apparent_Energy) \
    X(Power) X(Reactive_Power) X(Apparent_Power) X(Volume) X(Flow) X(Voltage) \
    X(Frequency) X(Pressure) X(PointInTime) X(RelativeHumidity) X(HCA) X(Text) \
    X(Angle) X(Dimensionless)

enum class Quantity : uint8_t {
#define X(n) n,
    WMB_LIST_OF_QUANTITIES
#undef X
    Unknown
};

// ---------------------------------------------------------------------------
// Units: X(cname, lcname, human, quantity)
// ---------------------------------------------------------------------------
#define WMB_LIST_OF_UNITS \
    X(Second, s, "s", Time) \
    X(M, m, "m", Length) \
    X(KG, kg, "kg", Mass) \
    X(Ampere, a, "A", Amperage) \
    X(K, k, "K", Temperature) \
    X(MOL, mol, "mol", AmountOfSubstance) \
    X(CD, cd, "cd", LuminousIntensity) \
    X(WH, wh, "Wh", Energy) \
    X(KWH, kwh, "kWh", Energy) \
    X(MJ, mj, "MJ", Energy) \
    X(GJ, gj, "GJ", Energy) \
    X(GCAL, gcal, "Gcal", Energy) \
    X(KVARH, kvarh, "kVARh", Reactive_Energy) \
    X(KVAH, kvah, "kVAh", Apparent_Energy) \
    X(M3C, m3c, "m3C", Energy) \
    X(W, w, "W", Power) \
    X(KW, kw, "kW", Power) \
    X(JH, jh, "J/h", Power) \
    X(MJH, mjh, "MJ/h", Power) \
    X(KVAR, kvar, "kVAR", Reactive_Power) \
    X(KVA, kva, "kVA", Apparent_Power) \
    X(M3CH, m3ch, "m3C/h", Power) \
    X(DBM, dbm, "dBm", Power) \
    X(M3, m3, "m3", Volume) \
    X(L, l, "l", Volume) \
    X(M3H, m3h, "m3/h", Flow) \
    X(LH, lh, "l/h", Flow) \
    X(C, c, "C", Temperature) \
    X(F, f, "F", Temperature) \
    X(Volt, v, "V", Voltage) \
    X(HZ, hz, "Hz", Frequency) \
    X(PA, pa, "Pa", Pressure) \
    X(BAR, bar, "bar", Pressure) \
    X(Minute, min, "min", Time) \
    X(Hour, h, "h", Time) \
    X(Day, d, "d", Time) \
    X(Month, month, "month", Time) \
    X(Year, y, "y", Time) \
    X(UnixTimestamp, ut, "ut", PointInTime) \
    X(DateTimeUTC, utc, "utc", PointInTime) \
    X(DateTimeLT, datetime, "datetime", PointInTime) \
    X(DateLT, date, "date", PointInTime) \
    X(TimeLT, time, "time", PointInTime) \
    X(RH, rh, "%RH", RelativeHumidity) \
    X(HCA, hca, "HCA", HCA) \
    X(TXT, txt, "", Text) \
    X(DEGREE, deg, "deg", Angle) \
    X(RADIAN, rad, "rad", Angle) \
    X(COUNTER, counter, "", Dimensionless) \
    X(FACTOR, factor, "", Dimensionless) \
    X(NUMBER, nr, "", Dimensionless) \
    X(PERCENTAGE, pct, "%", Dimensionless) \
    X(PPM, ppm, "ppm", Dimensionless)

enum class Unit : uint8_t {
#define X(cname, lcname, hr, q) cname,
    WMB_LIST_OF_UNITS
#undef X
    Unknown
};

// ---------------------------------------------------------------------------
// VIF ranges: X(name, from, to, quantity, default unit)
// ---------------------------------------------------------------------------
#define WMB_LIST_OF_VIF_RANGES \
    X(Volume, 0x10, 0x17, Volume, M3) \
    X(OnTime, 0x20, 0x23, Time, Hour) \
    X(OperatingTime, 0x24, 0x27, Time, Hour) \
    X(VolumeFlow, 0x38, 0x3F, Flow, M3H) \
    X(FlowTemperature, 0x58, 0x5B, Temperature, C) \
    X(ReturnTemperature, 0x5C, 0x5F, Temperature, C) \
    X(TemperatureDifference, 0x60, 0x63, Temperature, C) \
    X(ExternalTemperature, 0x64, 0x67, Temperature, C) \
    X(Pressure, 0x68, 0x6B, Pressure, BAR) \
    X(HeatCostAllocation, 0x6E, 0x6E, HCA, HCA) \
    X(Date, 0x6C, 0x6C, PointInTime, DateLT) \
    X(DateTime, 0x6D, 0x6D, PointInTime, DateTimeLT) \
    X(EnergyMJ, 0x08, 0x0F, Energy, MJ) \
    X(EnergyWh, 0x00, 0x07, Energy, KWH) \
    X(PowerW, 0x28, 0x2F, Power, KW) \
    X(PowerJh, 0x30, 0x37, Power, MJH) \
    X(ActualityDuration, 0x74, 0x77, Time, Hour) \
    X(FabricationNo, 0x78, 0x78, Text, TXT) \
    X(EnhancedIdentification, 0x79, 0x79, Text, TXT) \
    X(EnergyMWh, 0x7B00, 0x7B01, Energy, KWH) \
    X(EnergyGJ, 0x7B08, 0x7B09, Energy, MJ) \
    X(RelativeHumidity, 0x7B1A, 0x7B1B, RelativeHumidity, RH) \
    X(AccessNumber, 0x7D08, 0x7D08, Dimensionless, COUNTER) \
    X(Medium, 0x7D09, 0x7D09, Text, TXT) \
    X(Manufacturer, 0x7D0A, 0x7D0A, Text, TXT) \
    X(ParameterSet, 0x7D0B, 0x7D0B, Text, TXT) \
    X(ModelVersion, 0x7D0C, 0x7D0C, Text, TXT) \
    X(HardwareVersion, 0x7D0D, 0x7D0D, Text, TXT) \
    X(FirmwareVersion, 0x7D0E, 0x7D0E, Text, TXT) \
    X(SoftwareVersion, 0x7D0F, 0x7D0F, Text, TXT) \
    X(Location, 0x7D10, 0x7D10, Text, TXT) \
    X(Customer, 0x7D11, 0x7D11, Text, TXT) \
    X(ErrorFlags, 0x7D17, 0x7D17, Text, TXT) \
    X(DigitalOutput, 0x7D1A, 0x7D1A, Text, TXT) \
    X(DigitalInput, 0x7D1B, 0x7D1B, Text, TXT) \
    X(DurationSinceReadout, 0x7D2C, 0x7D2F, Time, Hour) \
    X(DurationOfTariff, 0x7D31, 0x7D33, Time, Hour) \
    X(Dimensionless, 0x7D3A, 0x7D3A, Dimensionless, COUNTER) \
    X(Voltage, 0x7D40, 0x7D4F, Voltage, Volt) \
    X(Amperage, 0x7D50, 0x7D5F, Amperage, Ampere) \
    X(ResetCounter, 0x7D60, 0x7D60, Dimensionless, COUNTER) \
    X(CumulationCounter, 0x7D61, 0x7D61, Dimensionless, COUNTER) \
    X(SpecialSupplierInformation, 0x7D67, 0x7D67, Text, TXT) \
    X(RemainingBattery, 0x7D74, 0x7D74, Time, Day) \
    X(AnyVolumeVIF, 0x00, 0x00, Volume, Unknown) \
    X(AnyEnergyVIF, 0x00, 0x00, Energy, Unknown) \
    X(AnyPowerVIF, 0x00, 0x00, Power, Unknown)

enum class VifRange : uint8_t {
    Any,
#define X(name, from, to, q, u) name,
    WMB_LIST_OF_VIF_RANGES
#undef X
    None
};

// ---------------------------------------------------------------------------
// VIF combinables (VIFE codes that modify the meaning of a VIF)
// ---------------------------------------------------------------------------
#define WMB_LIST_OF_VIF_COMBINABLES \
    X(Reserved, 0x00, 0x11) X(Average, 0x12, 0x12) X(InverseCompactProfile, 0x13, 0x13) \
    X(RelativeDeviation, 0x14, 0x14) X(RecordErrorCodeMeterToController, 0x15, 0x1c) \
    X(StandardConformantDataContent, 0x1d, 0x1d) X(CompactProfileWithRegister, 0x1e, 0x1e) \
    X(CompactProfile, 0x1f, 0x1f) X(PerSecond, 0x20, 0x20) X(PerMinute, 0x21, 0x21) \
    X(PerHour, 0x22, 0x22) X(PerDay, 0x23, 0x23) X(PerWeek, 0x24, 0x24) \
    X(PerMonth, 0x25, 0x25) X(PerYear, 0x26, 0x26) X(PerRevolutionMeasurement, 0x27, 0x27) \
    X(IncrPerInputPulseChannel0, 0x28, 0x28) X(IncrPerInputPulseChannel1, 0x29, 0x29) \
    X(IncrPerOutputPulseChannel0, 0x2a, 0x2a) X(IncrPerOutputPulseChannel1, 0x2b, 0x2b) \
    X(PerLitre, 0x2c, 0x2c) X(PerM3, 0x2d, 0x2d) X(PerKg, 0x2e, 0x2e) X(PerKelvin, 0x2f, 0x2f) \
    X(PerKWh, 0x30, 0x30) X(PerGJ, 0x31, 0x31) X(PerKW, 0x32, 0x32) \
    X(PerKelvinLitreW, 0x33, 0x33) X(PerVolt, 0x34, 0x34) X(PerAmpere, 0x35, 0x35) \
    X(MultipliedByS, 0x36, 0x36) X(MultipliedBySDivV, 0x37, 0x37) \
    X(MultipliedBySDivA, 0x38, 0x38) X(StartDateTimeOfAB, 0x39, 0x39) \
    X(UncorrectedMeterUnit, 0x3a, 0x3a) X(ForwardFlow, 0x3b, 0x3b) \
    X(BackwardFlow, 0x3c, 0x3c) X(ReservedNonMetric, 0x3d, 0x3d) \
    X(ValueAtBaseCondC, 0x3e, 0x3e) X(ObisDeclaration, 0x3f, 0x3f) \
    X(LowerLimit, 0x40, 0x40) X(ExceedsLowerLimit, 0x41, 0x41) \
    X(DateTimeExceedsLowerFirstBegin, 0x42, 0x42) X(DateTimeExceedsLowerFirstEnd, 0x43, 0x43) \
    X(DateTimeExceedsLowerLastBegin, 0x46, 0x46) X(DateTimeExceedsLowerLastEnd, 0x47, 0x47) \
    X(UpperLimit, 0x48, 0x48) X(ExceedsUpperLimit, 0x49, 0x49) \
    X(DateTimeExceedsUpperFirstBegin, 0x4a, 0x4a) X(DateTimeExceedsUpperFirstEnd, 0x4b, 0x4b) \
    X(DateTimeExceedsUpperLastBegin, 0x4d, 0x4d) X(DateTimeExceedsUpperLastEnd, 0x4e, 0x4e) \
    X(DurationExceedsLowerFirst, 0x50, 0x53) X(DurationExceedsLowerLast, 0x54, 0x57) \
    X(DurationExceedsUpperFirst, 0x58, 0x5b) X(DurationExceedsUpperLast, 0x5c, 0x5f) \
    X(DurationOfDFirst, 0x60, 0x63) X(DurationOfDLast, 0x64, 0x67) \
    X(ValueDuringLowerLimitExceeded, 0x68, 0x68) X(LeakageValues, 0x69, 0x69) \
    X(OverflowValues, 0x6a, 0x6a) X(ValueDuringUpperLimitExceeded, 0x6c, 0x6c) \
    X(DateTimeOfDEFirstBegin, 0x6a, 0x6a) X(DateTimeOfDEFirstEnd, 0x6b, 0x6b) \
    X(DateTimeOfDELastBegin, 0x6e, 0x6e) X(DateTimeOfDELastEnd, 0x6f, 0x6f) \
    X(MultiplicativeCorrectionFactorForValue, 0x70, 0x77) \
    X(AdditiveCorrectionConstant, 0x78, 0x7b) X(CombinableVIFExtension, 0x7c, 0x7c) \
    X(MultiplicativeCorrectionFactorForValue103, 0x7d, 0x7d) X(FutureValue, 0x7e, 0x7e) \
    X(MfctSpecific, 0x7f, 0x7f) X(AtPhase1, 0x7c01, 0x7c01) X(AtPhase2, 0x7c02, 0x7c02) \
    X(AtPhase3, 0x7c03, 0x7c03) X(AtNeutral, 0x7c04, 0x7c04) \
    X(BetweenPhaseL1AndL2, 0x7c05, 0x7c05) X(BetweenPhaseL2AndL3, 0x7c06, 0x7c06) \
    X(BetweenPhaseL3AndL1, 0x7c07, 0x7c07) X(AtQuadrantQ1, 0x7c08, 0x7c08) \
    X(AtQuadrantQ2, 0x7c09, 0x7c09) X(AtQuadrantQ3, 0x7c0a, 0x7c0a) \
    X(AtQuadrantQ4, 0x7c0b, 0x7c0b) X(DeltaBetweenImportAndExport, 0x7c0c, 0x7c0c) \
    X(AccumulationOfAbsoluteValue, 0x7c10, 0x7c10) X(DataPresentedWithTypeC, 0x7c11, 0x7c11) \
    X(DataPresentedWithTypeD, 0x7c12, 0x7c12) X(Mfct00, 0x7f00, 0x7f00) \
    X(Mfct01, 0x7f01, 0x7f01) X(Mfct02, 0x7f02, 0x7f02) X(Mfct03, 0x7f03, 0x7f03) \
    X(Mfct04, 0x7f04, 0x7f04) X(Mfct05, 0x7f05, 0x7f05) X(Mfct06, 0x7f06, 0x7f06) \
    X(Mfct07, 0x7f07, 0x7f07) X(Mfct08, 0x7f08, 0x7f08) X(Mfct21, 0x7f21, 0x7f21) \
    X(Mfct72, 0x7f72, 0x7f72) X(Synthetic, 0x7f77, 0x7f77)

enum class VifCombinable : uint8_t {
    Any,
#define X(name, from, to) name,
    WMB_LIST_OF_VIF_COMBINABLES
#undef X
    None
};

enum class MeasurementType : uint8_t { Instantaneous = 0, Maximum = 1, Minimum = 2, AtError = 3, Any = 4 };

// ---------------------------------------------------------------------------
// Table helpers (units.cpp / types.cpp)
// ---------------------------------------------------------------------------
const char* quantity_name(Quantity q);
Unit        quantity_default_unit(Quantity q);
const char* unit_suffix(Unit u);      // lower-case json suffix, "kwh"
const char* unit_human(Unit u);       // "kWh"
Quantity    unit_quantity(Unit u);

const char* vif_range_name(VifRange r);
Unit        vif_range_default_unit(VifRange r);
Quantity    vif_range_quantity(VifRange r);
VifRange    vif_to_range(uint16_t vif);          // None when not in any range
bool        vif_in_range(uint16_t vif, VifRange r);
VifCombinable combinable_from_raw(uint16_t raw); // named combinable for a raw VIFE code
const char* combinable_name(VifCombinable c);

// Canonical scale divisor, value = raw / vif_scale(vif). Matches upstream,
// including the odd -1000000 for unscalable VIFs.
double vif_scale(uint16_t vif);

// Unit conversion (linear SI scales, temperature offsets, dBm).
bool   units_convertible(Unit from, Unit to);
double unit_convert(double v, Unit from, Unit to);
// kWh may be displayed as kVARh/kVAh (M-Bus lacks those units).
bool   unit_override_conversion(Unit from, Unit to);

// Manufacturer code helpers.
void     mfct_to_str(uint16_t m, char out[4]);
uint16_t mfct_from_str(const char* s);
const char* mfct_name(uint16_t m);   // full name for well known manufacturers, else nullptr

// Media type (device type byte) names as used by wmbusmeters json.
const char* media_name(uint8_t type, uint16_t mfct);

} // namespace wmb
