#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/smgui.h>
#include <hydrasdr.h>

#ifdef __ANDROID__
#include <android_backend.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "hydrasdr_rfone_source",
    /* Description:     */ "HydraSDR RFOne source module for SDR++",
    /* Author:          */ "Ryzerth/B.VERNOUX",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class HydraSDR_RFOne_SourceModule : public ModuleManager::Instance {
public:
    HydraSDR_RFOne_SourceModule(std::string name) {
        this->name = name;

        sampleRate = 10000000.0;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        refresh();
        if (sampleRateList.size() > 0) {
            sampleRate = sampleRateList[0];
        }

        // Select device from config
        config.acquire();
        std::string devSerial = config.conf["device"];
        config.release();
        selectByString(devSerial);

        sigpath::sourceManager.registerSource("HydraSDR RFOne", &handler);
    }

    ~HydraSDR_RFOne_SourceModule() {
        stop(this);
        sigpath::sourceManager.unregisterSource("HydraSDR RFOne");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void refresh() {
#ifndef __ANDROID__
        devList.clear();
        devListTxt = "";

        uint64_t serials[256];
        int n = hydrasdr_list_devices(serials, 256);

        char buf[1024];
        for (int i = 0; i < n; i++) {
            sprintf(buf, "%016" PRIX64, serials[i]);
            devList.push_back(serials[i]);
            devListTxt += buf;
            devListTxt += '\0';
        }
#else
        // Check for device presence
        int vid, pid;
        devFd = backend::getDeviceFD(vid, pid, backend::HYDRASDR_RFONE_VIDPIDS);
        if (devFd < 0) { return; }

        // Get device info
        std::string fakeName = "HydraSDR RFOne USB";
        devList.push_back(0xDEADBEEF);
        devListTxt += fakeName;
        devListTxt += '\0';
#endif
    }

    void selectFirst() {
        if (devList.size() != 0) {
            selectBySerial(devList[0]);
        }
    }

    void selectByString(std::string serial) {
        char buf[1024];
        for (int i = 0; i < devList.size(); i++) {
            sprintf(buf, "%016" PRIX64, devList[i]);
            std::string str = buf;
            if (serial == str) {
                selectBySerial(devList[i]);
                return;
            }
        }
        selectFirst();
    }

    void selectBySerial(uint64_t serial) {
        hydrasdr_device* dev;
        try {
#ifndef __ANDROID__
            int err = hydrasdr_open_sn(&dev, serial);
#else
            int err = hydrasdr_open_fd(&dev, devFd);
#endif
            if (err != 0) {
                char buf[1024];
                sprintf(buf, "%016" PRIX64, serial);
                flog::error("Could not open HydraSDR {0}", buf);
                selectedSerial = 0;
                return;
            }
        }
        catch (const std::exception& e) {
            char buf[1024];
            sprintf(buf, "%016" PRIX64, serial);
            flog::error("Could not open HydraSDR {}", buf);
        }
        selectedSerial = serial;

        uint32_t sampleRates[256];
        hydrasdr_get_samplerates(dev, sampleRates, 0);
        int n = sampleRates[0];
        hydrasdr_get_samplerates(dev, sampleRates, n);
        sampleRateList.clear();
        sampleRateListTxt = "";
        for (int i = 0; i < n; i++) {
            sampleRateList.push_back(sampleRates[i]);
            sampleRateListTxt += getBandwdithScaled(sampleRates[i]);
            sampleRateListTxt += '\0';
        }

        char buf[1024];
        sprintf(buf, "%016" PRIX64, serial);
        selectedSerStr = std::string(buf);

        // Load config here
        config.acquire();
        bool created = false;
        if (!config.conf["devices"].contains(selectedSerStr)) {
            created = true;
            config.conf["devices"][selectedSerStr]["sampleRate"] = 10000000;
            config.conf["devices"][selectedSerStr]["gainMode"] = 0;
            config.conf["devices"][selectedSerStr]["sensitiveGain"] = 0;
            config.conf["devices"][selectedSerStr]["linearGain"] = 0;
            config.conf["devices"][selectedSerStr]["lnaGain"] = 0;
            config.conf["devices"][selectedSerStr]["mixerGain"] = 0;
            config.conf["devices"][selectedSerStr]["vgaGain"] = 0;
            config.conf["devices"][selectedSerStr]["lnaAgc"] = false;
            config.conf["devices"][selectedSerStr]["mixerAgc"] = false;
            config.conf["devices"][selectedSerStr]["biasT"] = false;
        }

        // Load sample rate
        srId = 0;
        sampleRate = sampleRateList[0];
        if (config.conf["devices"][selectedSerStr].contains("sampleRate")) {
            int selectedSr = config.conf["devices"][selectedSerStr]["sampleRate"];
            for (int i = 0; i < sampleRateList.size(); i++) {
                if (sampleRateList[i] == selectedSr) {
                    srId = i;
                    sampleRate = selectedSr;
                    break;
                }
            }
        }

        // Load gains
        if (config.conf["devices"][selectedSerStr].contains("gainMode")) {
            gainMode = config.conf["devices"][selectedSerStr]["gainMode"];
        }
        if (config.conf["devices"][selectedSerStr].contains("sensitiveGain")) {
            sensitiveGain = config.conf["devices"][selectedSerStr]["sensitiveGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("linearGain")) {
            linearGain = config.conf["devices"][selectedSerStr]["linearGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("lnaGain")) {
            lnaGain = config.conf["devices"][selectedSerStr]["lnaGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("mixerGain")) {
            mixerGain = config.conf["devices"][selectedSerStr]["mixerGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("vgaGain")) {
            vgaGain = config.conf["devices"][selectedSerStr]["vgaGain"];
        }
        if (config.conf["devices"][selectedSerStr].contains("lnaAgc")) {
            lnaAgc = config.conf["devices"][selectedSerStr]["lnaAgc"];
        }
        if (config.conf["devices"][selectedSerStr].contains("mixerAgc")) {
            mixerAgc = config.conf["devices"][selectedSerStr]["mixerAgc"];
        }

        // Load Bias-T
        if (config.conf["devices"][selectedSerStr].contains("biasT")) {
            biasT = config.conf["devices"][selectedSerStr]["biasT"];
        }

        config.release(created);

        hydrasdr_close(dev);
    }

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    static void menuSelected(void* ctx) {
        HydraSDR_RFOne_SourceModule* _this = (HydraSDR_RFOne_SourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        flog::info("HydraSDR_RFOne_SourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        HydraSDR_RFOne_SourceModule* _this = (HydraSDR_RFOne_SourceModule*)ctx;
        flog::info("HydraSDR_RFOne_SourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        HydraSDR_RFOne_SourceModule* _this = (HydraSDR_RFOne_SourceModule*)ctx;
        if (_this->running) { return; }
        if (_this->selectedSerial == 0) {
            flog::error("Tried to start HydraSDR source with null serial");
            return;
        }

#ifndef __ANDROID__
        int err = hydrasdr_open_sn(&_this->openDev, _this->selectedSerial);
#else
        int err = hydrasdr_open_fd(&_this->openDev, _this->devFd);
#endif
        if (err != 0) {
            char buf[1024];
            sprintf(buf, "%016" PRIX64, _this->selectedSerial);
            flog::error("Could not open HydraSDR {0}", buf);
            return;
        }

        hydrasdr_set_samplerate(_this->openDev, _this->sampleRateList[_this->srId]);
        hydrasdr_set_freq(_this->openDev, _this->freq);

        if (_this->gainMode == 0) {
            hydrasdr_set_lna_agc(_this->openDev, 0);
            hydrasdr_set_mixer_agc(_this->openDev, 0);
            hydrasdr_set_sensitivity_gain(_this->openDev, _this->sensitiveGain);
        }
        else if (_this->gainMode == 1) {
            hydrasdr_set_lna_agc(_this->openDev, 0);
            hydrasdr_set_mixer_agc(_this->openDev, 0);
            hydrasdr_set_linearity_gain(_this->openDev, _this->linearGain);
        }
        else if (_this->gainMode == 2) {
            if (_this->lnaAgc) {
                hydrasdr_set_lna_agc(_this->openDev, 1);
            }
            else {
                hydrasdr_set_lna_agc(_this->openDev, 0);
                hydrasdr_set_lna_gain(_this->openDev, _this->lnaGain);
            }
            if (_this->mixerAgc) {
                hydrasdr_set_mixer_agc(_this->openDev, 1);
            }
            else {
                hydrasdr_set_mixer_agc(_this->openDev, 0);
                hydrasdr_set_mixer_gain(_this->openDev, _this->mixerGain);
            }
            hydrasdr_set_vga_gain(_this->openDev, _this->vgaGain);
        }

        hydrasdr_set_rf_bias(_this->openDev, _this->biasT);

        hydrasdr_start_rx(_this->openDev, callback, _this);

        _this->running = true;
        flog::info("HydraSDR_RFOne_SourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        HydraSDR_RFOne_SourceModule* _this = (HydraSDR_RFOne_SourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;
        _this->stream.stopWriter();
        hydrasdr_close(_this->openDev);
        _this->stream.clearWriteStop();
        flog::info("HydraSDR_RFOne_SourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        HydraSDR_RFOne_SourceModule* _this = (HydraSDR_RFOne_SourceModule*)ctx;
        if (_this->running) {
            hydrasdr_set_freq(_this->openDev, freq);
        }
        _this->freq = freq;
        flog::info("HydraSDR_RFOne_SourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        HydraSDR_RFOne_SourceModule* _this = (HydraSDR_RFOne_SourceModule*)ctx;

        if (_this->running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_hydrasdr_dev_sel_", _this->name), &_this->devId, _this->devListTxt.c_str())) {
            _this->selectBySerial(_this->devList[_this->devId]);
            core::setInputSampleRate(_this->sampleRate);
            if (_this->selectedSerStr != "") {
                config.acquire();
                config.conf["device"] = _this->selectedSerStr;
                config.release(true);
            }
        }

        if (SmGui::Combo(CONCAT("##_hydrasdr_sr_sel_", _this->name), &_this->srId, _this->sampleRateListTxt.c_str())) {
            _this->sampleRate = _this->sampleRateList[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            if (_this->selectedSerStr != "") {
                config.acquire();
                config.conf["devices"][_this->selectedSerStr]["sampleRate"] = _this->sampleRate;
                config.release(true);
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_hydrasdr_refr_", _this->name))) {
            _this->refresh();
            config.acquire();
            std::string devSerial = config.conf["device"];
            config.release();
            _this->selectByString(devSerial);
            core::setInputSampleRate(_this->sampleRate);
        }

        if (_this->running) { SmGui::EndDisabled(); }

        SmGui::BeginGroup();
        SmGui::Columns(3, CONCAT("HydraSDRGainModeColumns##_", _this->name), false);
        SmGui::ForceSync();
        if (SmGui::RadioButton(CONCAT("Sensitive##_hydrasdr_gm_", _this->name), _this->gainMode == 0)) {
            _this->gainMode = 0;
            if (_this->running) {
                hydrasdr_set_lna_agc(_this->openDev, 0);
                hydrasdr_set_mixer_agc(_this->openDev, 0);
                hydrasdr_set_sensitivity_gain(_this->openDev, _this->sensitiveGain);
            }
            if (_this->selectedSerStr != "") {
                config.acquire();
                config.conf["devices"][_this->selectedSerStr]["gainMode"] = 0;
                config.release(true);
            }
        }
        SmGui::NextColumn();
        SmGui::ForceSync();
        if (SmGui::RadioButton(CONCAT("Linear##_hydrasdr_gm_", _this->name), _this->gainMode == 1)) {
            _this->gainMode = 1;
            if (_this->running) {
                hydrasdr_set_lna_agc(_this->openDev, 0);
                hydrasdr_set_mixer_agc(_this->openDev, 0);
                hydrasdr_set_linearity_gain(_this->openDev, _this->linearGain);
            }
            if (_this->selectedSerStr != "") {
                config.acquire();
                config.conf["devices"][_this->selectedSerStr]["gainMode"] = 1;
                config.release(true);
            }
        }
        SmGui::NextColumn();
        SmGui::ForceSync();
        if (SmGui::RadioButton(CONCAT("Free##_hydrasdr_gm_", _this->name), _this->gainMode == 2)) {
            _this->gainMode = 2;
            if (_this->running) {
                if (_this->lnaAgc) {
                    hydrasdr_set_lna_agc(_this->openDev, 1);
                }
                else {
                    hydrasdr_set_lna_agc(_this->openDev, 0);
                    hydrasdr_set_lna_gain(_this->openDev, _this->lnaGain);
                }
                if (_this->mixerAgc) {
                    hydrasdr_set_mixer_agc(_this->openDev, 1);
                }
                else {
                    hydrasdr_set_mixer_agc(_this->openDev, 0);
                    hydrasdr_set_mixer_gain(_this->openDev, _this->mixerGain);
                }
                hydrasdr_set_vga_gain(_this->openDev, _this->vgaGain);
            }
            if (_this->selectedSerStr != "") {
                config.acquire();
                config.conf["devices"][_this->selectedSerStr]["gainMode"] = 2;
                config.release(true);
            }
        }
        SmGui::Columns(1, CONCAT("EndHydraSDRGainModeColumns##_", _this->name), false);
        SmGui::EndGroup();

        // Gain menus

        if (_this->gainMode == 0) {
            SmGui::LeftLabel("Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_sens_gain_", _this->name), &_this->sensitiveGain, 0, 21)) {
                if (_this->running) {
                    hydrasdr_set_sensitivity_gain(_this->openDev, _this->sensitiveGain);
                }
                if (_this->selectedSerStr != "") {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["sensitiveGain"] = _this->sensitiveGain;
                    config.release(true);
                }
            }
        }
        else if (_this->gainMode == 1) {
            SmGui::LeftLabel("Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_lin_gain_", _this->name), &_this->linearGain, 0, 21)) {
                if (_this->running) {
                    hydrasdr_set_linearity_gain(_this->openDev, _this->linearGain);
                }
                if (_this->selectedSerStr != "") {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["linearGain"] = _this->linearGain;
                    config.release(true);
                }
            }
        }
        else if (_this->gainMode == 2) {
            // TODO: Switch to a table for alignment
            if (_this->lnaAgc) { SmGui::BeginDisabled(); }
            SmGui::LeftLabel("LNA Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_lna_gain_", _this->name), &_this->lnaGain, 0, 15)) {
                if (_this->running) {
                    hydrasdr_set_lna_gain(_this->openDev, _this->lnaGain);
                }
                if (_this->selectedSerStr != "") {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["lnaGain"] = _this->lnaGain;
                    config.release(true);
                }
            }
            if (_this->lnaAgc) { SmGui::EndDisabled(); }

            if (_this->mixerAgc) { SmGui::BeginDisabled(); }
            SmGui::LeftLabel("Mixer Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_mix_gain_", _this->name), &_this->mixerGain, 0, 15)) {
                if (_this->running) {
                    hydrasdr_set_mixer_gain(_this->openDev, _this->mixerGain);
                }
                if (_this->selectedSerStr != "") {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["mixerGain"] = _this->mixerGain;
                    config.release(true);
                }
            }
            if (_this->mixerAgc) { SmGui::EndDisabled(); }

            SmGui::LeftLabel("VGA Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_vga_gain_", _this->name), &_this->vgaGain, 0, 15)) {
                if (_this->running) {
                    hydrasdr_set_vga_gain(_this->openDev, _this->vgaGain);
                }
                if (_this->selectedSerStr != "") {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["vgaGain"] = _this->vgaGain;
                    config.release(true);
                }
            }

            // AGC Control
            SmGui::ForceSync();
            if (SmGui::Checkbox(CONCAT("LNA AGC##_hydrasdr_", _this->name), &_this->lnaAgc)) {
                if (_this->running) {
                    if (_this->lnaAgc) {
                        hydrasdr_set_lna_agc(_this->openDev, 1);
                    }
                    else {
                        hydrasdr_set_lna_agc(_this->openDev, 0);
                        hydrasdr_set_lna_gain(_this->openDev, _this->lnaGain);
                    }
                }
                if (_this->selectedSerStr != "") {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["lnaAgc"] = _this->lnaAgc;
                    config.release(true);
                }
            }
            SmGui::ForceSync();
            if (SmGui::Checkbox(CONCAT("Mixer AGC##_hydrasdr_", _this->name), &_this->mixerAgc)) {
                if (_this->running) {
                    if (_this->mixerAgc) {
                        hydrasdr_set_mixer_agc(_this->openDev, 1);
                    }
                    else {
                        hydrasdr_set_mixer_agc(_this->openDev, 0);
                        hydrasdr_set_mixer_gain(_this->openDev, _this->mixerGain);
                    }
                }
                if (_this->selectedSerStr != "") {
                    config.acquire();
                    config.conf["devices"][_this->selectedSerStr]["mixerAgc"] = _this->mixerAgc;
                    config.release(true);
                }
            }
        }

        // Bias T
        if (SmGui::Checkbox(CONCAT("Bias T##_hydrasdr_", _this->name), &_this->biasT)) {
            if (_this->running) {
                hydrasdr_set_rf_bias(_this->openDev, _this->biasT);
            }
            if (_this->selectedSerStr != "") {
                config.acquire();
                config.conf["devices"][_this->selectedSerStr]["biasT"] = _this->biasT;
                config.release(true);
            }
        }
    }

    static int callback(hydrasdr_transfer_t* transfer) {
        HydraSDR_RFOne_SourceModule* _this = (HydraSDR_RFOne_SourceModule*)transfer->ctx;
        memcpy(_this->stream.writeBuf, transfer->samples, transfer->sample_count * sizeof(dsp::complex_t));
        if (!_this->stream.swap(transfer->sample_count)) { return -1; }
        return 0;
    }

    std::string name;
    hydrasdr_device* openDev;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;
    uint64_t selectedSerial = 0;
    std::string selectedSerStr = "";
    int devId = 0;
    int srId = 0;

    bool biasT = false;

    int lnaGain = 0;
    int vgaGain = 0;
    int mixerGain = 0;
    int linearGain = 0;
    int sensitiveGain = 0;

    int gainMode = 0;

    bool lnaAgc = false;
    bool mixerAgc = false;

#ifdef __ANDROID__
    int devFd = 0;
#endif

    std::vector<uint64_t> devList;
    std::string devListTxt;
    std::vector<uint32_t> sampleRateList;
    std::string sampleRateListTxt;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["devices"] = json({});
    def["device"] = "";
    config.setPath(core::args["root"].s() + "/hydrasdr_rfone_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new HydraSDR_RFOne_SourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (HydraSDR_RFOne_SourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}