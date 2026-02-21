#include "effects/backends/builtin/whitenoiseeffect.h"

#include "control/controlproxy.h"
#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/rampingvalue.h"

namespace {
const QString dryWetParameterId = QStringLiteral("dry_wet");
const QString gainParameterId = QStringLiteral("gain");
const QString filterParameterId = QStringLiteral("filter");
const double kMinFreq = 17.0;
const double kMaxFreq = 22050.0;

float apply_deadzone(float value, float deadzone, float max_value){
    return std::max(0.0f, (value - deadzone) / (max_value - deadzone));
}

double map_value(double value, double input_from, double input_to,
        double output_from, double output_to){
    DEBUG_ASSERT(input_from != input_to);
    DEBUG_ASSERT(output_from != output_to);
    double normalized = (value - input_from) / (input_to - input_from);
    return output_from + normalized * (output_to - output_from);
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
    drywet->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    drywet->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    drywet->setDefaultLinkType(EffectManifestParameter::LinkType::None);
    drywet->setRange(0, 1, 1);

    // This is gain parameter
    EffectManifestParameterPointer gain = pManifest->addParameter();
    gain->setId(gainParameterId);
    gain->setName(QObject::tr("Gain"));
    gain->setDescription(QObject::tr("Gain for white noise"));
    gain->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    gain->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    gain->setDefaultLinkType(EffectManifestParameter::LinkType::None);
    gain->setRange(0, 1, 1);

    // This is filter parameter
    EffectManifestParameterPointer filter = pManifest->addParameter();
    filter->setId(filterParameterId);
    filter->setName(QObject::tr("Filter"));
    filter->setDescription(QObject::tr("Filter for white noise"));
    filter->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    filter->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    filter->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    filter->setRange(0, 0.5, 1);

    return pManifest;
}

void WhiteNoiseEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pDryWetParameter = parameters.value(dryWetParameterId);
    m_pGainParameter = parameters.value(gainParameterId);
    m_pFilterParameter = parameters.value(filterParameterId);
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

    const float drywet_deadzone = 0.01f;
    const float filter_deadzone = 0.05f;

    // Get dry/wet and filter control value and set up ramping
    CSAMPLE drywet = static_cast<CSAMPLE>(m_pDryWetParameter->value());
    CSAMPLE filter = static_cast<CSAMPLE>(m_pFilterParameter->value());
    
    drywet = apply_deadzone(drywet, drywet_deadzone, 1.0f);
    if(drywet > 0.0001 || gs.previous_drywet > 0.0001){
        RampingValue<CSAMPLE_GAIN> drywet_ramping_value(
                gs.previous_drywet, drywet, engineParameters.samplesPerBuffer());

        
        
        
        // Generate white noise
        std::uniform_real_distribution<> r_distributor(-1.0, 1.0);
        const auto bufferSize = engineParameters.samplesPerBuffer();
        for (unsigned int i = 0; i < bufferSize; ++i) {
            float noise = static_cast<float>(r_distributor(gs.gen));
            gs.m_noiseBuffer[i] = noise;
        }

        double hp_center_freq = kMinFreq;
        double lp_center_freq = kMaxFreq;

        if(filter < 0.5){
            hp_center_freq = map_value(filter, 0, 0.5, kMinFreq, kMaxFreq);
        }
        else
        {
            lp_center_freq = map_value(filter, 0.5, 1, kMinFreq, kMaxFreq);
        }

        gs.m_highpass.setFrequencyCorners(engineParameters.sampleRate(), hp_center_freq, 0.707106781);
        gs.m_lowpass.setFrequencyCorners(engineParameters.sampleRate(), lp_center_freq, 0.707106781);

        // Apply high-pass and low-pass filtering to the noise
        gs.m_highpass.process(gs.m_noiseBuffer.data(), gs.m_filteredBuffer.data(), bufferSize);
        gs.m_lowpass.process(gs.m_filteredBuffer.data(), gs.m_filteredBuffer.data(), bufferSize);

        // Get the master gain value
        CSAMPLE gain = static_cast<CSAMPLE>(m_pGainParameter->value());

        // Mix dry and wet signals, apply gain and ramp the dry/wet effect
        for (unsigned int i = 0; i < bufferSize; ++i) {
            CSAMPLE_GAIN drywet_ramped = drywet_ramping_value.getNth(i);

            // Apply the dry/wet control and gain to the output signal
            pOutput[i] = (pInput[i] * (1 - drywet_ramped) +
                                gs.m_filteredBuffer[i] * drywet_ramped) *
                    gain;
        }
    }
    else
    {
        SampleUtil::copy(pOutput, pInput, engineParameters.samplesPerBuffer());
    }

    // Store the current drywet value for the next buffer
    if (enableState == EffectEnableState::Disabling) {
        gs.previous_drywet = 0;
    } else {
        gs.previous_drywet = drywet;
    }
}
