#include "SN.hpp"

#include <math.h>

#include <iostream>

#include "../vmath/algebra.hpp"
#include "../vmath/convert.hpp"
#include "../vmath/loadtxt.hpp"
#include "../vmath/stat.hpp"

#include "utils.hpp"


// Initialises empty data structure
SN::SN() {}

// Single file initialisation, either spectra or light curve
SN::SN(std::string fileName) {
    // if fileName matches *.dat load as light curve
    if (utils::fileExtention(fileName) == "dat") {
        loadLC(fileName);

    } else {
        std::cout << "Unrecognised data type for file: " << fileName << std::endl;
    }
}


// Add a spectrum file and read the data
void SN::addSpec(std::string fileName, double mjd) {
    if (utils::fileExists(fileName)) {
        // Create a temporarty SpecData object
        SpecData sd;

        // Load data into a temporarty vector then assign to SpecData object
        std::vector< std::vector<double> > temp = vmath::loadtxt<double>(fileName, 2);
        sd.file_ = fileName;
        sd.rawWav_ = temp[0];
        sd.rawFlux_ = temp[1];
        sd.wav_ = sd.rawWav_;
        sd.flux_ = sd.rawFlux_;
        sd.mjd_ = mjd;

        // Find a normalazing factor for a spectrum
        sd.fluxNorm_ = vmath::mean<double>(sd.flux_);

        // Add the data object to the spec_ unordered_map accessed by MJD
        spec_[mjd] = sd;

        // Create a LC epoch corresponding to the spectrum_file
        addEpoch(mjd);
    }
}


void SN::saveSpec(double mjdZeroPhase, double scale) {
    utils::createDirectory("spectra");

    int phase;
    std::string sPhase;
    for (auto &spec : spec_) {
        ofstream specFile;
        phase = round(spec.second.mjd_ - mjdZeroPhase);
        if (phase < 0) {
            sPhase = "m" + std::to_string(abs(phase));
        } else if (phase > 0) {
            sPhase = "p" + std::to_string(phase);
        } else {
            sPhase = "max";
        }
        specFile.open("spectra/" + name_ + "." + sPhase + ".dat");

        for (size_t i = 0; i < spec.second.flux_.size(); ++i) {
            specFile << spec.second.wav_[i] << " ";
            specFile << spec.second.flux_[i] * scale << "\n";
        }

        specFile.close();
    }
}


// Create a slice though the light curves at a given MJD
void SN::addEpoch(double mjd) {
    // Check if any light curves are assigned
    if (!lc_.empty()) {
        // create a temporarty SNEpoch object
        std::vector<Obs> epoch;

        size_t idxNearest;
        for (auto &lc : lc_) {
            idxNearest = vmath::nearest(lc.second.mjd_, mjd);

            // Check if the data points is within one day from the spectrum
            if (!(fabs(lc.second.mjd_[idxNearest] - mjd) > 1)) {
                Obs obs;
                obs.mjd_ = mjd;
                obs.flux_ = lc.second.flux_[idxNearest];
                obs.fluxErr_ = lc.second.fluxErr_[idxNearest];
                obs.filter_ = lc.second.filter_;
                epoch.push_back(obs);
            }
        }

        // Add the light curve slice to epoch_ unordered_map accessed by MJD
        epoch_[mjd] = epoch;
    }
}


// Load light curve from an input data file
void SN::loadLC(std::string fileName) {
    if (utils::fileExists(fileName)) {
        name_ = utils::baseName(fileName);

        // Load the light curve into a temporarty 2D vector and split into 1D
        std::vector< std::vector<string> > temp;
        vmath::loadtxt<std::string>(fileName, 4, temp);
        _rawMJD = vmath::castString<double>(temp[0]);
        _rawFlux = vmath::castString<double>(temp[1]);
        _rawFluxErr = vmath::castString<double>(temp[2]);
        _rawFilter = temp[3];

        // Make a list of unique filters
        filterList_ = _rawFilter;
        utils::removeDuplicates<std::string>(filterList_);

        // Create data structure for each light curve. One per filter
        for (auto &flt : filterList_) {
            lc_[flt].name_ = name_;
            lc_[flt].filter_ = flt;
        }

        // Loop though all data points and assign to correct data vectors
        for (size_t i = 0; i < _rawFilter.size(); ++i) {
            lc_[_rawFilter[i]].completeMJD_.push_back(_rawMJD[i]);
            lc_[_rawFilter[i]].completeFlux_.push_back(_rawFlux[i]);
            lc_[_rawFilter[i]].completeFluxErr_.push_back(_rawFluxErr[i]);
        }

        // Set the full light curve as the working version
        restoreCompleteLC();
        setLCStats();
    }

    else {
        std::cout << fileName << " file does not exist" << std::endl;
    }
}


// Make a synthetic light curve from
void SN::synthesiseLC(const std::vector<std::string> &filterList,
                      std::shared_ptr<Filters> filters) {
    // Clear light curve data
    lc_.clear();
    epoch_.clear();

    // Create new light curves
    for (auto &flt : filterList_) {
        lc_[flt].name_ = name_;
        lc_[flt].filter_ = flt;
        lc_[flt].mjd_ = vector<double>(0);
        lc_[flt].flux_ = vector<double>(0);
        lc_[flt].fluxErr_ = vector<double>(spec_.size(), 0);
    }

    // Loop though each spectrum
    for (auto &spec : spec_) {
        filters->rescale(spec.second.wav_);

        // loop though each filter and append the LC
        std::vector<Obs> epoch;
        for (auto &flt : filterList) {
            lc_[flt].mjd_.push_back(spec.second.mjd_);
            lc_[flt].flux_.push_back(filters->flux(spec.second.flux_, flt));

            Obs obs;
            obs.mjd_ = spec.second.mjd_;
            obs.flux_ = lc_[flt].flux_.back();
            obs.fluxErr_ = 0;
            obs.filter_ = flt;
            epoch.push_back(obs);
        }
        epoch_[spec.second.mjd_] = epoch;
    }

    setLCStats();
}


// Move all spectra to a new chosen redshift
void SN::redshift(double zNew, std::shared_ptr<Cosmology> cosmology) {
    // Find the wavelength shift between the old and new redshift
    double shift = (1 + zNew) / (1 + z_);

    // Find the flux shift between the old and new redshift
    cosmology->set(z_);
    double scale = cosmology->lumDis_;
    cosmology->set(zNew);
    scale /= cosmology->lumDis_;

    for (auto &spec : spec_) {
        vmath::div(spec.second.wav_, shift);
        vmath::mult(spec.second.flux_, scale);
        spec.second.fluxNorm_ *= scale;
    }

    z_ = zNew;
}


void SN::restoreCompleteLC() {
    for (auto &lc : lc_) {
        lc.second.mjd_ = lc.second.completeMJD_;
        lc.second.flux_ = lc.second.completeFlux_;
        lc.second.fluxErr_ = lc.second.completeFluxErr_;
    }
}


void SN::setLCStats() {
    for (auto &lc : lc_) {
        lc.second.mjdMin_ = vmath::min<double>(lc.second.mjd_);
        lc.second.mjdMax_ = vmath::max<double>(lc.second.mjd_);
        lc.second.normalization_ = vmath::max<double>(lc.second.flux_);

        lc.second.t_ = vmath::sub<double>(lc.second.mjd_, lc.second.mjdMin_);
    }
}