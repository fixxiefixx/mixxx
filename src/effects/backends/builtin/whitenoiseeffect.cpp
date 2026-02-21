#include "effects/backends/builtin/whitenoiseeffect.h"

#include "control/controlproxy.h"
#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/rampingvalue.h"

namespace {
const QString dryWetParameterId = QStringLiteral("dry_wet");
const QString qParameterId = QStringLiteral("q");
const double kMinFreq = 100.0;
const double kMaxFreq = 22050.0;

template<typename T>
requires std::is_floating_point_v<T>
T map_value(T value, T input_from, T input_to, T output_from, T output_to){
    DEBUG_ASSERT(input_from != input_to);
    DEBUG_ASSERT(output_from != output_to);
    T normalized = (value - input_from) / (input_to - input_from);
    return output_from + normalized * (output_to - output_from);
}

inline double interpolate_log(double x,
        double fMin,
        double fMax){
    // Clamp input to [0, 1] for safety
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;

    // Logarithmic interpolation
    return fMin * std::pow(fMax / fMin, x);
}


} // anonymous namespace

// static
QString WhiteNoiseEffect::getId() {
    return QStringLiteral("org.mixxx.effects.whitenoise");
}

// static
EffectManifestPointer WhiteNoiseEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("White Noise"));
    pManifest->setAuthor("The Mixxx Team");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr("Mix white noise with the input signal"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(0.5);

    // This is dry/wet parameter
    EffectManifestParameterPointer drywet = pManifest->addParameter();
    drywet->setId(dryWetParameterId);
    drywet->setName(QObject::tr("Dry/Wet"));
    drywet->setDescription(QObject::tr("Crossfade the noise with the dry signal"));
    drywet->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    drywet->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    drywet->setDefaultLinkType(EffectManifestParameter::LinkType::None);
    drywet->setRange(0, 0.5, 1);

    EffectManifestParameterPointer q = pManifest->addParameter();
    q->setId("q");
    q->setName(QObject::tr("Resonance"));
    q->setShortName(QObject::tr("Q"));
    q->setDescription(QObject::tr("Resonance of the filters"));
    q->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    q->setUnitsHint(EffectManifestParameter::UnitsHint::SampleRate);
    q->setRange(0.4, 1.3, 4.0);
    
    return pManifest;
}

void WhiteNoiseEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pDryWetParameter = parameters.value(dryWetParameterId);
    m_pQParameter = parameters.value(qParameterId);
}

void WhiteNoiseEffect::processChannel(
        WhiteNoiseGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);

    WhiteNoiseGroupState& gs = *pState;

    const CSAMPLE_GAIN drywet_deadzone = 0.01f;

    // Get dry/wet and filter control value and set up ramping
    CSAMPLE_GAIN drywet = static_cast<CSAMPLE_GAIN>(m_pDryWetParameter->value());
    double q = m_pQParameter->value();
    
    CSAMPLE_GAIN drywet_deadzoned = drywet;

    if(enableState == EffectEnableState::Disabling){
        drywet_deadzoned = 0.5f;
    }
    else{
        if(drywet_deadzoned  >= 0.5f){
            if(drywet_deadzoned < 0.5f + drywet_deadzone){
                drywet_deadzoned = 0.5f;
            }
            else{
                drywet_deadzoned = map_value(drywet_deadzoned, 0.5f + drywet_deadzone, 1.0f, 0.5f, 1.0f);
            }
        }
        else{
            if(drywet_deadzoned > 0.5f - drywet_deadzone){
                drywet_deadzoned = 0.5f;
            }
            else{
                drywet_deadzoned = map_value(drywet_deadzoned, 0.0f, 1.0f - drywet_deadzone, 0.0f, 1.0f);
            }
        }
    }

    // Get the master gain value
    CSAMPLE_GAIN gain = std::min(1.0f, std::abs(drywet_deadzoned - 0.5f) * 2.0f);
    //CSAMPLE_GAIN gain = std::min(1.0, interpolate_log(std::abs(drywet_deadzoned - 0.5f) * 2.0, 0.0, 1.0));

    if(gain > 0.0001 || gs.previous_gain > 0.0001){
        RampingValue<CSAMPLE_GAIN> gain_ramping_value(
                gs.previous_gain, gain, engineParameters.samplesPerBuffer());

        // Generate white noise
        std::uniform_real_distribution<> r_distributor(-1.0, 1.0);
        const auto bufferSize = engineParameters.samplesPerBuffer();
        for (unsigned int i = 0; i < bufferSize; ++i) {
            float noise = static_cast<float>(r_distributor(gs.gen));
            gs.m_noiseBuffer[i] = noise;
        }

        double hp_center_freq = kMinFreq;
        double lp_center_freq = kMaxFreq;

        if(drywet < 0.5){
            //hp_center_freq = map_value((double)drywet, 0.0, 0.5, kMinFreq, kMaxFreq);
            hp_center_freq = std::clamp( interpolate_log(drywet * 2.0, kMinFreq, kMaxFreq), kMinFreq, kMaxFreq);
        }
        else
        {
            //lp_center_freq = map_value((double)drywet, 0.5, 1.0, kMinFreq, kMaxFreq);
            lp_center_freq = std::clamp(interpolate_log((drywet - 0.5) * 2.0, kMinFreq, kMaxFreq), kMinFreq, kMaxFreq);
        }

        gs.m_highpass.setFrequencyCorners(engineParameters.sampleRate(), hp_center_freq, q);
        gs.m_lowpass.setFrequencyCorners(engineParameters.sampleRate(), lp_center_freq, q);

        // Apply high-pass and low-pass filtering to the noise
        gs.m_highpass.process(gs.m_noiseBuffer.data(), gs.m_filteredBuffer.data(), bufferSize);
        gs.m_lowpass.process(gs.m_filteredBuffer.data(), gs.m_filteredBuffer.data(), bufferSize);

        


        // Mix dry and wet signals, apply gain and ramp the dry/wet effect
        for (unsigned int i = 0; i < bufferSize; ++i) {
            CSAMPLE_GAIN gain_ramped = gain_ramping_value.getNth(i);

            // Apply gain to the output signal
            pOutput[i] = (pInput[i] * (1 - gain_ramped) +
                                gs.m_filteredBuffer[i] * gain_ramped);
        }
    }
    else
    {
        SampleUtil::copy(pOutput, pInput, engineParameters.samplesPerBuffer());
    }

    // Store the current drywet value for the next buffer
    gs.previous_gain = gain;
    gs.previous_q = q;
}
