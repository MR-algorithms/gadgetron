
#include "GenericReconCartesianReferencePrepGadget.h"
#include <iomanip>

#include "hoNDArray_reductions.h"
#include "mri_core_def.h"

namespace Gadgetron {

    GenericReconCartesianReferencePrepGadget::GenericReconCartesianReferencePrepGadget() : BaseClass()
    {
    }

    GenericReconCartesianReferencePrepGadget::~GenericReconCartesianReferencePrepGadget()
    {
    }

    int GenericReconCartesianReferencePrepGadget::process_config(ACE_Message_Block* mb)
    {
        GADGET_CHECK_RETURN(BaseClass::process_config(mb) == GADGET_OK, GADGET_FAIL);

        ISMRMRD::IsmrmrdHeader h;
        try
        {
            deserialize(mb->rd_ptr(), h);
        }
        catch (...)
        {
            GDEBUG("Error parsing ISMRMRD Header");
        }

        if (!h.acquisitionSystemInformation)
        {
            GDEBUG("acquisitionSystemInformation not found in header. Bailing out");
            return GADGET_FAIL;
        }

        // -------------------------------------------------

        size_t NE = h.encoding.size();
        num_encoding_spaces_ = NE;
        GDEBUG_CONDITION_STREAM(verbose.value(), "Number of encoding spaces: " << NE);

        calib_mode_.resize(NE, ISMRMRD_noacceleration);
        ref_prepared_.resize(NE, false);

        for (size_t e = 0; e < h.encoding.size(); e++)
        {
            ISMRMRD::EncodingSpace e_space = h.encoding[e].encodedSpace;
            ISMRMRD::EncodingSpace r_space = h.encoding[e].reconSpace;
            ISMRMRD::EncodingLimits e_limits = h.encoding[e].encodingLimits;

            GDEBUG_CONDITION_STREAM(verbose.value(), "---> Encoding space : " << e << " <---");
            GDEBUG_CONDITION_STREAM(verbose.value(), "Encoding matrix size: " << e_space.matrixSize.x << " " << e_space.matrixSize.y << " " << e_space.matrixSize.z);
            GDEBUG_CONDITION_STREAM(verbose.value(), "Encoding field_of_view : " << e_space.fieldOfView_mm.x << " " << e_space.fieldOfView_mm.y << " " << e_space.fieldOfView_mm.z);
            GDEBUG_CONDITION_STREAM(verbose.value(), "Recon matrix size : " << r_space.matrixSize.x << " " << r_space.matrixSize.y << " " << r_space.matrixSize.z);
            GDEBUG_CONDITION_STREAM(verbose.value(), "Recon field_of_view :  " << r_space.fieldOfView_mm.x << " " << r_space.fieldOfView_mm.y << " " << r_space.fieldOfView_mm.z);

            if (!h.encoding[e].parallelImaging)
            {
                GDEBUG_STREAM("Parallel Imaging section not found in header");
                calib_mode_[e] = ISMRMRD_noacceleration;
            }
            else
            {

                ISMRMRD::ParallelImaging p_imaging = *h.encoding[0].parallelImaging;
                GDEBUG_CONDITION_STREAM(verbose.value(), "acceFactorE1 is " << p_imaging.accelerationFactor.kspace_encoding_step_1);
                GDEBUG_CONDITION_STREAM(verbose.value(), "acceFactorE2 is " << p_imaging.accelerationFactor.kspace_encoding_step_2);

                std::string calib = *p_imaging.calibrationMode;

                bool separate = (calib.compare("separate") == 0);
                bool embedded = (calib.compare("embedded") == 0);
                bool external = (calib.compare("external") == 0);
                bool interleaved = (calib.compare("interleaved") == 0);
                bool other = (calib.compare("other") == 0);

                calib_mode_[e] = Gadgetron::ISMRMRD_noacceleration;
                if (p_imaging.accelerationFactor.kspace_encoding_step_1 > 1 || p_imaging.accelerationFactor.kspace_encoding_step_2 > 1)
                {
                    if (interleaved)
                        calib_mode_[e] = Gadgetron::ISMRMRD_interleaved;
                    else if (embedded)
                        calib_mode_[e] = Gadgetron::ISMRMRD_embedded;
                    else if (separate)
                        calib_mode_[e] = Gadgetron::ISMRMRD_separate;
                    else if (external)
                        calib_mode_[e] = Gadgetron::ISMRMRD_external;
                    else if (other)
                        calib_mode_[e] = Gadgetron::ISMRMRD_other;
                }
            }
        }

        return GADGET_OK;
    }

    int GenericReconCartesianReferencePrepGadget::process(Gadgetron::GadgetContainerMessage< IsmrmrdReconData >* m1)
    {
        if (perform_timing.value()) { gt_timer_.start("GenericReconCartesianReferencePrepGadget::process"); }

        process_called_times_++;

        IsmrmrdReconData* recon_bit_ = m1->getObjectPtr();
        if (recon_bit_->rbit_.size() > num_encoding_spaces_)
        {
            GWARN_STREAM("Incoming recon_bit has more encoding spaces than the protocol : " << recon_bit_->rbit_.size() << " instead of " << num_encoding_spaces_);
        }

        // for every encoding space, prepare the recon_bit_->rbit_[e].ref_
        size_t e;
        for (e = 0; e < recon_bit_->rbit_.size(); e++)
        {
            auto & rbit = recon_bit_->rbit_[e];
            std::stringstream os;
            os << "_encoding_" << e;

            // -----------------------------------------
            // no acceleration mode
            // check the availability of ref data
            // -----------------------------------------
            if (prepare_ref_always.value() || !ref_prepared_[e])
            {
                // if no ref data is set, make copy the ref point from the  data
                if (!rbit.ref_)
                {
                    rbit.ref_ = rbit.data_;
                    ref_prepared_[e] = true;
                }
            }
            else
            {
                if (ref_prepared_[e])
                {
                    if (rbit.ref_)
                    {
                        // remove the ref
                        rbit.ref_ = boost::none;
                    }
                }

                continue;
            }

            if (!rbit.ref_) continue;

            //if (!debug_folder_full_path_.empty())
            //{
            //    this->gt_exporter_.export_array_complex(rbit.ref_->data_, debug_folder_full_path_ + "ref_data" + os.str());
            //}

            // useful variables
            hoNDArray< std::complex<float> >& ref = (*rbit.ref_).data_;

            SamplingLimit sampling_limits[3];
            for (int i = 0; i < 3; i++)
            	sampling_limits[i] = (*rbit.ref_).sampling_.sampling_limits_[i];

            size_t RO = ref.get_size(0);
            size_t E1 = ref.get_size(1);
            size_t E2 = ref.get_size(2);
            size_t CHA = ref.get_size(3);
            size_t N = ref.get_size(4);
            size_t S = ref.get_size(5);
            size_t SLC = ref.get_size(6);

            // stored the ref data ready for calibration
            hoNDArray< std::complex<float> > ref_calib;
           // -----------------------------------------
            // 1) average the ref according to the input parameters; 
            //    if interleaved mode, sampling times for every E1/E2 location is detected and line by line averaging is performed 
            //    this is required when irregular cartesian sampling is used or number of frames cannot be divided in full by acceleration factor
            // 2) detect the sampled region and crop the ref data if needed
            // 3) update the sampling_limits
            // -----------------------------------------

            hoNDArray< std::complex<float> > ref_recon_buf;

            // step 1
            bool count_sampling_freq = (calib_mode_[e] == ISMRMRD_interleaved);
            GADGET_CHECK_EXCEPTION_RETURN(Gadgetron::compute_averaged_data_N_S(ref, average_all_ref_N.value(), average_all_ref_S.value(), count_sampling_freq, ref_calib), GADGET_FAIL);

            //if (!debug_folder_full_path_.empty())
            //{
            //    this->gt_exporter_.export_array_complex(ref_calib, debug_folder_full_path_ + "ref_calib" + os.str());
            //}

            // step 2, detect sampled region in ref, along E1 and E2
            size_t start_E1(0), end_E1(0);
            auto t = Gadgetron::detect_sampled_region_E1(ref);
            start_E1 = std::get<0>(t);
            end_E1 = std::get<1>(t);

            size_t start_E2(0), end_E2(0);
            if (E2 > 1)
            {
                auto t = Gadgetron::detect_sampled_region_E2(ref);
                start_E2 = std::get<0>(t);
                end_E2 = std::get<1>(t);
            }

            // crop the ref_calib, along RO, E1 and E2 for seperate or embedded mode
            vector_td<size_t, 3> crop_offset;
            crop_offset[0] = sampling_limits[0].min_;
            crop_offset[1] = start_E1;
            crop_offset[2] = start_E2;

            vector_td<size_t, 3> crop_size;
            crop_size[0] = sampling_limits[0].max_ - sampling_limits[0].min_ + 1;
            crop_size[1] = end_E1 - start_E1 + 1;
            crop_size[2] = end_E2 - start_E2 + 1;

            if (crop_size[0]> (ref_calib.get_size(0) - crop_offset[0]))
            {
                crop_size[0] = ref_calib.get_size(0) - crop_offset[0];
            }

            if (crop_size[1]> (ref_calib.get_size(1) - crop_offset[1]))
            {
                crop_size[1] = ref_calib.get_size(1) - crop_offset[1];
            }

            if (crop_size[2]> (ref_calib.get_size(2) - crop_offset[2]))
            {
                crop_size[2] = ref_calib.get_size(2) - crop_offset[2];
            }

            Gadgetron::crop(crop_offset, crop_size, &ref_calib, &ref_recon_buf);
            ref_calib = ref_recon_buf;

            //if (!debug_folder_full_path_.empty())
            //{
            //    this->gt_exporter_.export_array_complex(ref_calib, debug_folder_full_path_ + "ref_calib_after_crop" + os.str());
            //}

            // step 3, update the sampling limits
            sampling_limits[0].center_ = (uint16_t)(RO/2);

            sampling_limits[1].min_ = 0;
            sampling_limits[1].max_ = (uint16_t)(end_E1 - start_E1);

            sampling_limits[2].min_ = 0;
            sampling_limits[2].max_ = (uint16_t)(end_E2 - start_E2);

            if ( (calib_mode_[e] == Gadgetron::ISMRMRD_interleaved) || (calib_mode_[e] == Gadgetron::ISMRMRD_noacceleration) )
            {
                // need to keep the ref kspace center information
                sampling_limits[1].center_ = (uint16_t)(sampling_limits[1].center_ - start_E1);
                sampling_limits[2].center_ = (uint16_t)(sampling_limits[2].center_ - start_E2);
            }
            else
            {
                // sepearate, embedded mode, the ref center is the kspace center
                sampling_limits[1].center_ = (sampling_limits[1].max_ + 1) / 2;
                sampling_limits[2].center_ = (sampling_limits[2].max_ + 1) / 2;
            }

            if(sampling_limits[0].max_>=RO)
            {
                sampling_limits[0].max_ = RO - 1;
            }

            ref = ref_calib;
            ref_prepared_[e] = true;

            for (int i = 0; i < 3; i++)
                (*rbit.ref_).sampling_.sampling_limits_[i] = sampling_limits[i];

            //if (!debug_folder_full_path_.empty())
            //{
            //    this->gt_exporter_.export_array_complex(rbit.ref_->data_, debug_folder_full_path_ + "ref_calib_final" + os.str());
            //}
        }

        if (this->next()->putq(m1) < 0)
        {
            GERROR_STREAM("Put IsmrmrdReconData to Q failed ... ");
            return GADGET_FAIL;
        }

        if (perform_timing.value()) { gt_timer_.stop(); }

        return GADGET_OK;
    }

    GADGET_FACTORY_DECLARE(GenericReconCartesianReferencePrepGadget)
}
