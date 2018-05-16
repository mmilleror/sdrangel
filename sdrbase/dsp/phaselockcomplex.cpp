///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2018 F4EXB                                                      //
// written by Edouard Griffiths                                                  //
//                                                                               //
// See: http://liquidsdr.org/blog/pll-howto/                                     //
// Fixed filter registers saturation                                             //
// Added order for PSK locking. This brilliant idea actually comes from this     //
// post: https://www.dsprelated.com/showthread/comp.dsp/36356-1.php              //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <complex.h>
#include <math.h>
#include "phaselockcomplex.h"

PhaseLockComplex::PhaseLockComplex() :
    m_a1(1.0),
    m_a2(1.0),
    m_b0(1.0),
    m_b1(1.0),
    m_b2(1.0),
    m_v0(0.0),
    m_v1(0.0),
    m_v2(0.0),
    m_deltaPhi(0.0),
    m_phiHat(0.0),
    m_phiHatPrev(0.0),
    m_phiHat1(0.0),
    m_phiHat2(0.0),
    m_dPhiHatAccum(0.0),
    m_phiHatCount(0),
    m_y(1.0, 0.0),
    m_p(1.0, 0.0),
    m_yRe(1.0),
    m_yIm(0.0),
    m_freq(0.0),
    m_freqPrev(0.0),
    m_lock(0.0),
    m_lockCount(0),
    m_pskOrder(1),
    m_lockTime1(480),
    m_lockTime(2400),
    m_lockTimef(2400.0f),
    m_lockThreshold(4.8f),
    m_avgF(2400)
{
}

void PhaseLockComplex::computeCoefficients(Real wn, Real zeta, Real K)
{
    double t1 = K/(wn*wn);          //
    double t2 = 2*zeta/wn - 1/K;   //

    double b0 = 2*K*(1.+t2/2.0f);
    double b1 = 2*K*2.;
    double b2 = 2*K*(1.-t2/2.0f);

    double a0 =  1 + t1/2.0f;
    double a1 = -t1;
    double a2 = -1 + t1/2.0f;

    qDebug("PhaseLockComplex::computeCoefficients: b_raw: %f %f %f", b0, b1, b2);
    qDebug("PhaseLockComplex::computeCoefficients: a_raw: %f %f %f", a0, a1, a2);

    m_b0 = b0 / a0;
    m_b1 = b1 / a0;
    m_b2 = b2 / a0;

    //    a0 =  1.0  is implied
    m_a1 = a1 / a0;
    m_a2 = a2 / a0;

    qDebug("PhaseLockComplex::computeCoefficients: b: %f %f %f", m_b0, m_b1, m_b2);
    qDebug("PhaseLockComplex::computeCoefficients: a: 1.0 %f %f", m_a1, m_a2);

    reset();
}

void PhaseLockComplex::setPskOrder(unsigned int order)
{
    m_pskOrder = order > 0 ? order : 1;
    reset();
}

void PhaseLockComplex::setSampleRate(unsigned int sampleRate)
{
    m_lockTime1 = sampleRate / 100; // 10ms for order 1
    m_lockTime = sampleRate / 20; // 50ms for order > 1
    m_lockTimef = (float) m_lockTime;
    m_lockThreshold = m_lockTime * 0.00015f; // threshold of 0.002 taking division by lock time into account
    m_avgF.resize(m_lockTime);
    reset();
}

void PhaseLockComplex::reset()
{
    // reset filter accumulators and phase
    m_v0 = 0.0f;
    m_v1 = 0.0f;
    m_v2 = 0.0f;
    m_deltaPhi = 0.0f;
    m_phiHat = 0.0f;
    m_phiHatPrev = 0.0f;
    m_phiHat1 = 0.0f;
    m_phiHat2 = 0.0f;
    m_dPhiHatAccum = 0.0f;
    m_phiHatCount = 0;
    m_y.real(1.0);
    m_y.imag(0.0);
    m_p.real(1.0);
    m_p.imag(0.0);
    m_yRe = 1.0f;
    m_yIm = 0.0f;
    m_freq = 0.0f;
    m_freqPrev = 0.0f;
    m_lock = 0.0f;
    m_lockCount = 0;
}

void PhaseLockComplex::feedFLL(float re, float im)
{
    m_yRe = cos(m_phiHat);
    m_yIm = sin(m_phiHat);
    std::complex<float> y(m_yRe, m_yIm);
    std::complex<float> x(re, im);
    std::complex<float> p = x * m_y;
    float cross = m_p.real()*p.imag() - p.real()*m_p.imag();
    float dot = m_p.real()*p.real() + m_p.imag()*p.imag();
    float eF = cross * (dot < 0 ? -1 : 1); // frequency error

    m_freq += eF;       // correct instantaneous frequency
    m_phiHat += eF;     // advance phase with instantaneous frequency
    m_p = p;            // store previous product
}

void PhaseLockComplex::feed(float re, float im)
{
    m_yRe = cos(m_phiHat);
    m_yIm = sin(m_phiHat);
    m_y.real(m_yRe);
    m_y.imag(m_yIm);
    std::complex<float> x(re, im);
    m_deltaPhi = std::arg(x * std::conj(m_y));

    // bring phase 0 on any of the PSK symbols
    if (m_pskOrder > 1) {
        m_deltaPhi = normalizeAngle(m_pskOrder*m_deltaPhi);
    }

    // advance buffer
    m_v2 = m_v1;  // shift center register to upper register
    m_v1 = m_v0;  // shift lower register to center register

    // compute new lower register
    m_v0 = m_deltaPhi - m_v1*m_a1 - m_v2*m_a2;

    // compute new output
    m_phiHat = m_v0*m_b0 + m_v1*m_b1 + m_v2*m_b2;

    // prevent saturation
    if (m_phiHat > 2.0*M_PI)
    {
        m_v0 *= (m_phiHat - 2.0*M_PI) / m_phiHat;
        m_v1 *= (m_phiHat - 2.0*M_PI) / m_phiHat;
        m_v2 *= (m_phiHat - 2.0*M_PI) / m_phiHat;
        m_phiHat -= 2.0*M_PI;
    }

    if (m_phiHat < -2.0*M_PI)
    {
        m_v0 *= (m_phiHat + 2.0*M_PI) / m_phiHat;
        m_v1 *= (m_phiHat + 2.0*M_PI) / m_phiHat;
        m_v2 *= (m_phiHat + 2.0*M_PI) / m_phiHat;
        m_phiHat += 2.0*M_PI;
    }

    // lock estimation
    if (m_pskOrder > 1)
    {
        float dPhi = normalizeAngle(m_phiHat - m_phiHatPrev);

        m_avgF(dPhi);

        if (m_phiHatCount < (m_lockTime-1))
        {
            m_phiHatCount++;
        }
        else
        {
            m_freq = m_avgF.asFloat();
            float dFreq = m_freq - m_freqPrev;

            if ((dFreq > -m_lockThreshold) && (dFreq < m_lockThreshold))
            {
                if (m_lockCount < 20) {
                    m_lockCount++;
                }
            }
            else{
                if (m_lockCount > 0) {
                    m_lockCount--;
                }                
            }

            m_freqPrev = m_freq;
            m_phiHatCount = 0;
        }

        // if (m_phiHatCount < (m_lockTime-1))
        // {
        //     m_dPhiHatAccum += dPhi; // re-accumulate phase for differential calculation
        //     m_phiHatCount++;
        // }
        // else
        // {
        //     float dPhi11 = (m_dPhiHatAccum - m_phiHat1); // optimized out division by lock time
        //     float dPhi12 = (m_phiHat1 - m_phiHat2);
        //     m_lock = dPhi11 - dPhi12; // second derivative of phase to get lock status

        //     if ((m_lock > -m_lockThreshold) && (m_lock < m_lockThreshold)) // includes re-multiplication by lock time
        //     {
        //         if (m_lockCount < 20) { // [0..20]
        //             m_lockCount++;
        //         }
        //     }
        //     else
        //     {
        //         if (m_lockCount > 0) {
        //             m_lockCount -= 2;
        //         }
        //     }

        //     m_phiHat2 = m_phiHat1;
        //     m_phiHat1 = m_dPhiHatAccum;
        //     m_dPhiHatAccum = 0.0f;
        //     m_phiHatCount = 0;
        // }

        // m_phiHatPrev = m_phiHat;
    }
    else
    {
        m_freq = (m_phiHat - m_phiHatPrev) / (2.0*M_PI);

        if (m_freq < -1.0f) {
            m_freq += 2.0f;
        } else if (m_freq > 1.0f) {
            m_freq -= 2.0f;
        }

        float dFreq = m_freq - m_freqPrev;

        if ((dFreq > -0.01) && (dFreq < 0.01))
        {
            if (m_lockCount < (m_lockTime1-1)) { // [0..479]
                m_lockCount++;
            }
        }
        else
        {
            m_lockCount = 0;
        }

        m_phiHatPrev = m_phiHat;
        m_freqPrev = m_freq;
    }
}

float PhaseLockComplex::normalizeAngle(float angle)
{
    while (angle <= -M_PI) {
        angle += 2.0*M_PI;
    }
    while (angle > M_PI) {
        angle -= 2.0*M_PI;
    }
    return angle;
}