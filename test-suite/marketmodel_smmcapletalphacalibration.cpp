/* -*- mode: c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 Copyright (C) 2007 Ferdinando Ametrano
 Copyright (C) 2007 Marco Bianchetti
 Copyright (C) 2007 Cristina Duminuco
 Copyright (C) 2007 Mark Joshi

 This file is part of QuantLib, a free-software/open-source library
 for financial quantitative analysts and developers - http://quantlib.org/

 QuantLib is free software: you can redistribute it and/or modify it
 under the terms of the QuantLib license.  You should have received a
 copy of the license along with this program; if not, please email
 <quantlib-dev@lists.sf.net>. The license is also available online at
 <http://quantlib.org/license.shtml>.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the license for more details.
*/

#include "utilities.hpp"
#include <ql/models/marketmodels/models/capletcoterminalalphacalibration.hpp>
#include <ql/models/marketmodels/correlations/cotswapfromfwdcorrelation.hpp>
#include <ql/models/marketmodels/correlations/timehomogeneousforwardcorrelation.hpp>
#include <ql/models/marketmodels/models/piecewiseconstantabcdvariance.hpp>
#include <ql/models/marketmodels/models/capletcoterminalswaptioncalibration.hpp>
#include <ql/models/marketmodels/models/cotswaptofwdadapter.hpp>
#include <ql/models/marketmodels/models/pseudorootfacade.hpp>
#include <ql/models/marketmodels/products/multistep/multistepcoterminalswaps.hpp>
#include <ql/models/marketmodels/products/multistep/multistepcoterminalswaptions.hpp>
#include <ql/models/marketmodels/products/multistep/multistepswap.hpp>
#include <ql/models/marketmodels/products/multiproductcomposite.hpp>
#include <ql/models/marketmodels/accountingengine.hpp>
#include <ql/models/marketmodels/utilities.hpp>
#include <ql/models/marketmodels/evolvers/lognormalcotswapratepc.hpp>
#include <ql/models/marketmodels/evolvers/lognormalfwdratepc.hpp>
#include <ql/models/marketmodels/correlations/expcorrelations.hpp>
#include <ql/models/marketmodels/models/flatvol.hpp>
#include <ql/models/marketmodels/models/abcdvol.hpp>
#include <ql/models/marketmodels/browniangenerators/mtbrowniangenerator.hpp>
#include <ql/models/marketmodels/browniangenerators/sobolbrowniangenerator.hpp>
#include <ql/models/marketmodels/swapforwardmappings.hpp>
#include <ql/models/marketmodels/curvestates/coterminalswapcurvestate.hpp>
#include <ql/methods/montecarlo/genericlsregression.hpp>
#include <ql/legacy/libormarketmodels/lmlinexpcorrmodel.hpp>
#include <ql/legacy/libormarketmodels/lmextlinexpvolmodel.hpp>
#include <ql/time/schedule.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/simpledaycounter.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/utilities/dataformatters.hpp>
#include <ql/math/integrals/segmentintegral.hpp>
#include <ql/math/statistics/convergencestatistics.hpp>
#include <ql/math/functional.hpp>
#include <ql/math/optimization/simplex.hpp>
#include <ql/math/statistics/sequencestatistics.hpp>
#include <sstream>

#include <float.h>

using namespace QuantLib;


#define BEGIN(x) (x+0)
#define END(x) (x+LENGTH(x))

namespace {

    Date todaysDate_, startDate_, endDate_;
    std::vector<Time> rateTimes_;
    std::vector<Real> accruals_;
    Calendar calendar_;
    DayCounter dayCounter_;
    std::vector<Rate> todaysForwards_, todaysSwaps_;
    std::vector<Real> coterminalAnnuity_;
    Size numberOfFactors_;
    Real alpha_, alphaMax_, alphaMin_;
    Spread displacement_;
    std::vector<DiscountFactor> todaysDiscounts_;
    std::vector<Volatility> swaptionDisplacedVols_, swaptionVols_;
    std::vector<Volatility> capletDisplacedVols_, capletVols_;
    Real a_, b_, c_, d_;
    Real longTermCorrelation_, beta_;
    Size measureOffset_;
    unsigned long seed_;
    Size paths_, trainingPaths_;
    bool printReport_ = false;

    void setup() {

        // Times
        calendar_ = NullCalendar();
        todaysDate_ = Settings::instance().evaluationDate();
        //startDate = todaysDate + 5*Years;
        endDate_ = todaysDate_ + 66*Months;
        Schedule dates(todaysDate_, endDate_, Period(Semiannual),
                       calendar_, Following, Following, DateGeneration::Backward, false);
        rateTimes_ = std::vector<Time>(dates.size()-1);
        accruals_ = std::vector<Real>(rateTimes_.size()-1);
        dayCounter_ = SimpleDayCounter();
        for (Size i=1; i<dates.size(); ++i)
            rateTimes_[i-1] = dayCounter_.yearFraction(todaysDate_, dates[i]);
        for (Size i=1; i<rateTimes_.size(); ++i)
            accruals_[i-1] = rateTimes_[i] - rateTimes_[i-1];

        // Rates & displacement
        todaysForwards_ = std::vector<Rate>(accruals_.size());
        numberOfFactors_ = 3;
        alpha_ = 0.0;
        alphaMax_ = 1.0;
        alphaMin_ = -1.0;
        displacement_ = 0.0;
        for (Size i=0; i<todaysForwards_.size(); ++i) {
            todaysForwards_[i] = 0.03 + 0.0025*i;
            //    todaysForwards_[i] = 0.03;
        }
        LMMCurveState curveState_lmm(rateTimes_);
        curveState_lmm.setOnForwardRates(todaysForwards_);
        todaysSwaps_ = curveState_lmm.coterminalSwapRates();

        // Discounts
        todaysDiscounts_ = std::vector<DiscountFactor>(rateTimes_.size());
        todaysDiscounts_[0] = 0.95;
        for (Size i=1; i<rateTimes_.size(); ++i)
            todaysDiscounts_[i] = todaysDiscounts_[i-1] /
                (1.0+todaysForwards_[i-1]*accruals_[i-1]);

        //// Swaption Volatilities
        //Volatility mktSwaptionVols[] = {
        //                        0.15541283,
        //                        0.18719678,
        //                        0.20890740,
        //                        0.22318179,
        //                        0.23212717,
        //                        0.23731450,
        //                        0.23988649,
        //                        0.24066384,
        //                        0.24023111,
        //                        0.23900189,
        //                        0.23726699,
        //                        0.23522952,
        //                        0.23303022,
        //                        0.23076564,
        //                        0.22850101,
        //                        0.22627951,
        //                        0.22412881,
        //                        0.22206569,
        //                        0.22009939
        //};

        //a = -0.0597;
        //b =  0.1677;
        //c =  0.5403;
        //d =  0.1710;

        a_ = 0.0;
        b_ = 0.17;
        c_ = 1.0;
        d_ = 0.10;

        Volatility mktCapletVols[] = {
            0.1640,
            0.1740,
            0.1840,
            0.1940,
            0.1840,
            0.1740,
            0.1640,
            0.1540,
            0.1440,
            0.1340376439125532
        };

        //swaptionDisplacedVols = std::vector<Volatility>(todaysSwaps.size());
        //swaptionVols = std::vector<Volatility>(todaysSwaps.size());
        //capletDisplacedVols = std::vector<Volatility>(todaysSwaps.size());
        capletVols_.resize(todaysSwaps_.size());
        for (Size i=0; i<todaysSwaps_.size(); i++) {
            //    swaptionDisplacedVols[i] = todaysSwaps[i]*mktSwaptionVols[i]/
            //                              (todaysSwaps[i]+displacement);
            //    swaptionVols[i]= mktSwaptionVols[i];
            //    capletDisplacedVols[i] = todaysForwards[i]*mktCapletVols[i]/
            //                            (todaysForwards[i]+displacement);
            capletVols_[i]= mktCapletVols[i];
        }

        // Cap/Floor Correlation
        longTermCorrelation_ = 0.5;
        beta_ = 0.2;
        measureOffset_ = 5;

        // Monte Carlo
        seed_ = 42;

#ifdef _DEBUG
        paths_ = 127;
        trainingPaths_ = 31;
#else
        paths_ = 32767; //262144-1; //; // 2^15-1
        trainingPaths_ = 8191; // 2^13-1
#endif
    }

    enum MarketModelType { ExponentialCorrelationFlatVolatility,
                           ExponentialCorrelationAbcdVolatility/*,
                           CalibratedMM*/
    };

    enum MeasureType { ProductSuggested, Terminal,
                       MoneyMarket, MoneyMarketPlus };

    enum EvolverType { Ipc, Pc , NormalPc};

}


TEST_CASE( "MarketModelSmmCapletAlphaCalibration_Function", "[MarketModelSmmCapletAlphaCalibration]" ) {

    INFO("Testing alpha caplet calibration "
                       "in a lognormal coterminal swap market model...");

    setup();

    Size numberOfRates = todaysForwards_.size();

    EvolutionDescription evolution(rateTimes_);
    // Size numberOfSteps = evolution.numberOfSteps();

    std::shared_ptr<PiecewiseConstantCorrelation> fwdCorr(new
        ExponentialForwardCorrelation(rateTimes_,
                                      longTermCorrelation_,
                                      beta_));

    std::shared_ptr<LMMCurveState> cs(new LMMCurveState(rateTimes_));
    cs->setOnForwardRates(todaysForwards_);

    std::shared_ptr<PiecewiseConstantCorrelation> corr(new
        CotSwapFromFwdCorrelation(fwdCorr, *cs, displacement_));

    std::vector<std::shared_ptr<PiecewiseConstantVariance> >
                                    swapVariances(numberOfRates);
    for (Size i=0; i<numberOfRates; ++i) {
        swapVariances[i] = std::shared_ptr<PiecewiseConstantVariance>(new
            PiecewiseConstantAbcdVariance(a_, b_, c_, d_,
                                          i, rateTimes_));
    }

    // create calibrator
    std::vector<Real> alphaInitial(numberOfRates, alpha_);
    std::vector<Real> alphaMax(numberOfRates,  1.0);
    std::vector<Real> alphaMin(numberOfRates, -1.0);
    bool maximizeHomogeneity = false; //?
    if (printReport_) {
        INFO("caplet market vols: " << QL_FIXED <<
                           std::setprecision(4) << io::sequence(capletVols_));
        INFO("alphaMin:           " << alphaMin_);
        INFO("alphaInitial:       " << alpha_);
        INFO("alphaMax:           " << alphaMax_);
        INFO("maximizeHomogeneity:" << maximizeHomogeneity);
    }
    CTSMMCapletAlphaFormCalibration calibrator(evolution,
                                               corr,
                                               swapVariances,
                                               capletVols_,
                                               cs,
                                               displacement_,
                                               alphaInitial,
                                               alphaMax,
                                               alphaMin,
                                               maximizeHomogeneity);
    // calibrate
    Natural maxIterations = 10;
    Real capletTolerance = 1e-4; // i.e. 1 bp
    Natural innerMaxIterations = 100;
    Real innerTolerance = 1e-8;

    if (printReport_) {
        INFO("numberOfFactors:    " << numberOfFactors_);
        INFO("maxIterations:      " << maxIterations);
        INFO("capletTolerance:    " << io::rate(capletTolerance));
        INFO("innerMaxIterations: " << innerMaxIterations);
        INFO("innerTolerance:     " << io::rate(innerTolerance));
    }

    bool result = calibrator.calibrate(numberOfFactors_,
                                       maxIterations,
                                       capletTolerance,
                                       innerMaxIterations,
                                       innerTolerance);
    if (!result)
        FAIL_CHECK("calibration failed");

    const std::vector<Matrix>& swapPseudoRoots = calibrator.swapPseudoRoots();
    std::shared_ptr<MarketModel> smm(new
        PseudoRootFacade(swapPseudoRoots,
                         rateTimes_,
                         cs->coterminalSwapRates(),
                         std::vector<Spread>(numberOfRates, displacement_)));
    CotSwapToFwdAdapter flmm(smm);
    Matrix capletTotCovariance = flmm.totalCovariance(numberOfRates-1);

    std::vector<Volatility> capletVols(numberOfRates);
    for (Size i=0; i<numberOfRates; ++i) {
        capletVols[i] = std::sqrt(capletTotCovariance[i][i]/rateTimes_[i]);
    }
    if (printReport_) {
        INFO("caplet smm implied vols: " << QL_FIXED <<
                           std::setprecision(4) << io::sequence(capletVols));
        INFO("failures: " << calibrator.failures());
        INFO("deformationSize: " << calibrator.deformationSize());
        INFO("capletRmsError: " << calibrator.capletRmsError());
        INFO("capletMaxError: " << calibrator.capletMaxError());
        INFO("swaptionRmsError: " << calibrator.swaptionRmsError());
        INFO("swaptionMaxError: " << calibrator.swaptionMaxError());
    }

    // check perfect swaption fit
    Real error, swapTolerance = 1e-14;
    Matrix swapTerminalCovariance(numberOfRates, numberOfRates, 0.0);
    for (Size i=0; i<numberOfRates; ++i) {
        Volatility expSwaptionVol = swapVariances[i]->totalVolatility(i);
        swapTerminalCovariance += swapPseudoRoots[i] * transpose(swapPseudoRoots[i]);
        Volatility swaptionVol = std::sqrt(swapTerminalCovariance[i][i]/rateTimes_[i]);
        error = std::fabs(swaptionVol-expSwaptionVol);
        if (error>swapTolerance)
            FAIL_CHECK("failed to reproduce " << io::ordinal(i+1) << " swaption vol:"
                        "\n expected:  " << io::rate(expSwaptionVol) <<
                        "\n realized:  " << io::rate(swaptionVol) <<
                        "\n error:     " << error <<
                        "\n tolerance: " << swapTolerance);
    }

    // check caplet fit
    for (Size i=0; i<numberOfRates; ++i) {
        error = std::fabs(capletVols[i]-capletVols_[i]);
        if (error>capletTolerance)
            FAIL_CHECK("failed to reproduce " << io::ordinal(i+1) << " caplet vol:"
                        "\n expected:         " << io::rate(capletVols_[i]) <<
                        "\n realized:         " << io::rate(capletVols[i]) <<
                        "\n percentage error: " << error/capletVols_[i] <<
                        "\n error:            " << error <<
                        "\n tolerance:        " << capletTolerance);
    }
}