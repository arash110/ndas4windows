/*++

Copyright (c) 1989-2000 Microsoft Corporation

Module Name:

    FspDisp.c

Abstract:

    This module implements the main dispatch procedure/thread for the Fat
    Fsp


--*/

#include "FatProcs.h"

//
//  Internal support routine, spinlock wrapper.
//

PVOID
FatRemoveOverflowEntry (
    IN PVOLUME_DEVICE_OBJECT VolDo
    );

//
//  Define our local debug trace level
//

#define Dbg                              (DEBUG_TRACE_FSP_DISPATCHER)

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FatFspDispatch)
#endif


VOID
FatFspDispatch (
    IN PVOID Context
    )

/*++

Routine Description:

    This is the main FSP thread routine that is executed to receive
    and dispatch IRP requests.  Each FSP thread begins its execution here.
    There is one thread created at system initialization time and subsequent
    threads created as needed.

Arguments:


    Context - Supplies the thread id.

Return Value:

    None - This routine never exits

--*/

{
    NTSTATUS Status;

    PIRP Irp;
    PIRP_CONTEXT IrpContext;
    PIO_STACK_LOCATION IrpSp;
    BOOLEAN VcbDeleted;

    PVOLUME_DEVICE_OBJECT VolDo;

    PAGED_CODE();

    IrpContext = (PIRP_CONTEXT)Context;

    Irp = IrpContext->OriginatingIrp;

    IrpSp = IoGetCurrentIrpStackLocation( Irp );

    //
    //  Now because we are the Fsp we will force the IrpContext to
    //  indicate true on Wait.
    //

    SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP);

    //
    //  If this request has an associated volume device object, remember it.
    //

    if ( IrpSp->FileObject != NULL ) {

        VolDo = CONTAINING_RECORD( IrpSp->DeviceObject,
                                   VOLUME_DEVICE_OBJECT,
                                   DeviceObject );
    } else {

        VolDo = NULL;
    }

    //
    //  Now case on the function code.  For each major function code,
    //  either call the appropriate FSP routine or case on the minor
    //  function and then call the FSP routine.  The FSP routine that
    //  we call is responsible for completing the IRP, and not us.
    //  That way the routine can complete the IRP and then continue
    //  post processing as required.  For example, a read can be
    //  satisfied right away and then read can be done.
    //
    //  We'll do all of the work within an exception handler that
    //  will be invoked if ever some underlying operation gets into
    //  trouble (e.g., if FatReadSectorsSync has trouble).
    //

    while ( TRUE ) {

        DebugTrace(0, Dbg, "FatFspDispatch: Irp = 0x%08lx\n", Irp);

        //
        //  If this Irp was top level, note it in our thread local storage.
        //

        FsRtlEnterFileSystem();

        if ( FlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_RECURSIVE_CALL) ) {

            IoSetTopLevelIrp( (PIRP)FSRTL_FSP_TOP_LEVEL_IRP );

        } else {

            IoSetTopLevelIrp( Irp );
        }

#if __NDAS_FAT__

		do {

			BOOLEAN	volDoResourceAcquired = FALSE;

			try {

				if (IrpContext->MajorFunction == IRP_MJ_CREATE) {
				
					if (VolDo->Secondary == NULL) {
						
						Status = FatCommonCreate( IrpContext, Irp );
					
					} else if (IrpContext->PrimaryRequestInfo.PrimarySessionId != NDASFAT_LOCAL_PRMARY_SESSION_ID	&&
							   IrpContext->PrimaryRequestInfo.PrimarySession->SessionContext.Uid == VolDo->Secondary->Thread.SessionContext.Uid	&&
							   IrpContext->PrimaryRequestInfo.PrimarySession->SessionContext.Tid == VolDo->Secondary->Thread.SessionContext.Tid) {
				
						Status = FatCommonCreate( IrpContext, Irp );
		
					} else {
						
						BOOLEAN	secondaryResourceAcquired = FALSE;
						BOOLEAN secondaryRecoveryResourceAcquired = FALSE;

						NDAS_ASSERT( FatIsTopLevelRequest(IrpContext) );

						Status = STATUS_SUCCESS;

						for (;;) {

							PIO_STACK_LOCATION	irpSp = IoGetCurrentIrpStackLocation(Irp);

							NDAS_ASSERT( secondaryRecoveryResourceAcquired == FALSE );
							NDAS_ASSERT( secondaryResourceAcquired == FALSE );

							if (!FlagOn(VolDo->Vcb.VcbState, VCB_STATE_FLAG_LOCKED)) {
					
								if (FlagOn(VolDo->Secondary->Thread.Flags, SECONDARY_THREAD_FLAG_REMOTE_DISCONNECTED)) {

									secondaryRecoveryResourceAcquired 
										= SecondaryAcquireResourceExclusiveLite( IrpContext, 
																				 &VolDo->RecoveryResource, 
																				 BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) );
								
									if (!FlagOn(VolDo->Secondary->Thread.Flags, SECONDARY_THREAD_FLAG_REMOTE_DISCONNECTED) ) {
	
										SecondaryReleaseResourceLite( IrpContext, &VolDo->RecoveryResource );
										secondaryRecoveryResourceAcquired = FALSE;
										continue;
									}

									secondaryResourceAcquired 
										= SecondaryAcquireResourceExclusiveLite( IrpContext, 
																				 &VolDo->Resource, 
																				 BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) );
									try {
								
										SecondaryRecoverySessionStart( VolDo->Secondary, IrpContext );
								
									} finally {

										SecondaryReleaseResourceLite( IrpContext, &VolDo->Resource );
										secondaryResourceAcquired = FALSE;

										SecondaryReleaseResourceLite( IrpContext, &VolDo->RecoveryResource );
										secondaryRecoveryResourceAcquired = FALSE;
									}

									continue;
								}
							}

							if (irpSp->FileObject->FileName.Length == (sizeof(WAKEUP_VOLUME_FILE_NAME)-sizeof(WCHAR)) && 
								RtlEqualMemory(irpSp->FileObject->FileName.Buffer, WAKEUP_VOLUME_FILE_NAME, irpSp->FileObject->FileName.Length)) {

								secondaryResourceAcquired 
									= SecondaryAcquireResourceSharedLite( IrpContext, 
																		  &VolDo->Resource, 
																		  FALSE );

								if (secondaryResourceAcquired == FALSE) {

									FatRaiseStatus( IrpContext, STATUS_OBJECT_NAME_NOT_FOUND );
								}

								if (FlagOn(VolDo->Secondary->Thread.Flags, SECONDARY_THREAD_FLAG_REMOTE_DISCONNECTED) ||
									FlagOn(VolDo->Secondary->Flags, SECONDARY_FLAG_RECONNECTING)) {

									SecondaryReleaseResourceLite( IrpContext, &VolDo->Resource );
									secondaryResourceAcquired = FALSE;

									FatRaiseStatus( IrpContext, STATUS_OBJECT_NAME_NOT_FOUND );
								}
					
							} else {	

								secondaryResourceAcquired 
									= SecondaryAcquireResourceSharedLite( IrpContext, 
																		  &VolDo->Resource, 
																		  BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) );
							}

							NDAS_ASSERT( secondaryResourceAcquired == TRUE );

							break;
						}

						if (Status == STATUS_SUCCESS) {
			
							try {

								if (FlagOn(IrpContext->NdasFatFlags, NDAS_FAT_IRP_CONTEXT_FLAG_ALREADY_OPENED_BY_PRIMARY_SESSION)) {

									PLIST_ENTRY			primarySessionListEntry;
									PPRIMARY_SESSION	primarySession;

									for (primarySession = NULL, primarySessionListEntry = VolDo->PrimarySessionQueue.Flink; 
										 primarySessionListEntry != &VolDo->PrimarySessionQueue; 
										 primarySession = NULL, primarySessionListEntry = primarySessionListEntry->Flink) {
			
										primarySession = CONTAINING_RECORD( primarySessionListEntry, PRIMARY_SESSION, ListEntry );
	
										if (IrpContext->FileOpenPrimarySessionId == primarySession->PrimarySessionId) {

											if (primarySession->SessionContext.Uid != VolDo->Secondary->Thread.SessionContext.Uid ||
												primarySession->SessionContext.Tid != VolDo->Secondary->Thread.SessionContext.Tid) {

												primarySession = NULL;
											}

											break;
										}
									}

									if (primarySession == NULL) {

										FatRaiseStatus( IrpContext, STATUS_ACCESS_DENIED );
									}

									ClearFlag( IrpContext->NdasFatFlags, NDAS_FAT_IRP_CONTEXT_FLAG_ALREADY_OPENED_BY_PRIMARY_SESSION );
									SetFlag( IrpContext->NdasFatFlags, NDAS_FAT_IRP_CONTEXT_FLAG_OPEN_TRY_ON_SECONDARY );
								}
                                
								Status = FatCommonCreate( IrpContext, Irp );
							
							} finally {

								NDAS_ASSERT( ExIsResourceAcquiredSharedLite(&VolDo->Resource) );
								SecondaryReleaseResourceLite( NULL, &VolDo->Resource );
							}
						}
					}

					goto Next;
				}

				Status = STATUS_SUCCESS;

				if (Irp && IS_SECONDARY_FILEOBJECT(IoGetCurrentIrpStackLocation(Irp)->FileObject)) {

					BOOLEAN volDoRecoveryResourceAcquired = FALSE;

					ASSERT( FatIsTopLevelRequest(IrpContext) );

					NDAS_ASSERT( FlagOn(IrpContext->NdasFatFlags, NDAS_FAT_IRP_CONTEXT_FLAG_SECONDARY_CONTEXT) );

					for (;;) {
			
						NDAS_ASSERT( volDoRecoveryResourceAcquired == FALSE );
						NDAS_ASSERT( volDoResourceAcquired == FALSE );

						if (FlagOn(VolDo->Secondary->Thread.Flags, SECONDARY_THREAD_FLAG_REMOTE_DISCONNECTED)) {

							volDoRecoveryResourceAcquired 
								= SecondaryAcquireResourceExclusiveLite( IrpContext, 
																		 &VolDo->RecoveryResource, 
																		 BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) );
								
							if (!FlagOn(VolDo->Secondary->Thread.Flags, SECONDARY_THREAD_FLAG_REMOTE_DISCONNECTED) ) {

								SecondaryReleaseResourceLite( IrpContext, &VolDo->RecoveryResource );
								volDoRecoveryResourceAcquired = FALSE;
								continue;
							}

							volDoResourceAcquired 
								= SecondaryAcquireResourceExclusiveLite( IrpContext, 
																		 &VolDo->Resource, 
																		 BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) );
							try {
								
								SecondaryRecoverySessionStart( VolDo->Secondary, IrpContext );
									
							} finally {

								SecondaryReleaseResourceLite( IrpContext, &VolDo->Resource );
								volDoResourceAcquired = FALSE;

								SecondaryReleaseResourceLite( IrpContext, &VolDo->RecoveryResource );
								volDoRecoveryResourceAcquired = FALSE;
							}

							continue;
						}

						volDoResourceAcquired 
							= SecondaryAcquireResourceSharedLite( IrpContext, 
																  &VolDo->Resource, 
																  BooleanFlagOn(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT) );

						NDAS_ASSERT( volDoResourceAcquired == TRUE );

						break;
					}
				}

	            switch ( IrpContext->MajorFunction ) {

		            //
			        //  For Create Operation,
				    //

					case IRP_MJ_CREATE:

						(VOID) FatCommonCreate( IrpContext, Irp );
	                    break;

		            //
			        //  For close operations.  We do a little kludge here in case
				    //  this close causes a volume to go away.  It will NULL the
					//  VolDo local variable so that we will not try to look at
	                //  the overflow queue.
		            //

	                case IRP_MJ_CLOSE:

		            {
			            PVCB Vcb;
				        PFCB Fcb;
					    PCCB Ccb;
						TYPE_OF_OPEN TypeOfOpen;

	                    //
		                //  Extract and decode the file object
			            //

				        TypeOfOpen = FatDecodeFileObject( IrpSp->FileObject, &Vcb, &Fcb, &Ccb );

					    //
						//  Do the close.  We have a slightly different format
	                    //  for this call because of the async closes.
		                //

			            Status = FatCommonClose( Vcb,
				                                 Fcb,
					                             Ccb,
						                         TypeOfOpen,
							                     TRUE,
								                 &VcbDeleted );

	                    //
		                //  If the VCB was deleted, do not try to access it later.
			            //

				        if (VcbDeleted) {

					        VolDo = NULL;
						}

	                    ASSERT(Status == STATUS_SUCCESS);

		                FatCompleteRequest( IrpContext, Irp, Status );

			            break;
				    }

	                //
		            //  For read operations
			        //

				    case IRP_MJ_READ:

					    (VOID) FatCommonRead( IrpContext, Irp );
						break;

	                //
		            //  For write operations,
			        //

				    case IRP_MJ_WRITE:

					    (VOID) FatCommonWrite( IrpContext, Irp );
						break;

	                //
		            //  For Query Information operations,
			        //

				    case IRP_MJ_QUERY_INFORMATION:

					    (VOID) FatCommonQueryInformation( IrpContext, Irp );
						break;

	                //
		            //  For Set Information operations,
			        //

				    case IRP_MJ_SET_INFORMATION:

					    (VOID) FatCommonSetInformation( IrpContext, Irp );
						break;

	                //
		            //  For Query EA operations,
			        //

				    case IRP_MJ_QUERY_EA:

					    (VOID) FatCommonQueryEa( IrpContext, Irp );
						break;

	                //
		            //  For Set EA operations,
			        //

				    case IRP_MJ_SET_EA:

					    (VOID) FatCommonSetEa( IrpContext, Irp );
						break;

	                //
		            //  For Flush buffers operations,
			        //

				    case IRP_MJ_FLUSH_BUFFERS:

					    (VOID) FatCommonFlushBuffers( IrpContext, Irp );
						break;

	                //
		            //  For Query Volume Information operations,
			        //

				    case IRP_MJ_QUERY_VOLUME_INFORMATION:

					    (VOID) FatCommonQueryVolumeInfo( IrpContext, Irp );
						break;

	                //
		            //  For Set Volume Information operations,
			        //

				    case IRP_MJ_SET_VOLUME_INFORMATION:

					    (VOID) FatCommonSetVolumeInfo( IrpContext, Irp );
						break;

	                //
		            //  For File Cleanup operations,
			        //

				    case IRP_MJ_CLEANUP:

					    (VOID) FatCommonCleanup( IrpContext, Irp );
						break;

	                //
		            //  For Directory Control operations,
			        //

				    case IRP_MJ_DIRECTORY_CONTROL:

					    (VOID) FatCommonDirectoryControl( IrpContext, Irp );
						break;

	                //
		            //  For File System Control operations,
			        //

				    case IRP_MJ_FILE_SYSTEM_CONTROL:

					    (VOID) FatCommonFileSystemControl( IrpContext, Irp );
						break;

	                //
		            //  For Lock Control operations,
			        //

				    case IRP_MJ_LOCK_CONTROL:

					    (VOID) FatCommonLockControl( IrpContext, Irp );
						break;

	                //
		            //  For Device Control operations,
			        //

				    case IRP_MJ_DEVICE_CONTROL:

					    (VOID) FatCommonDeviceControl( IrpContext, Irp );
						break;

	                //
		            //  For the Shutdown operation,
			        //

				    case IRP_MJ_SHUTDOWN:

					    (VOID) FatCommonShutdown( IrpContext, Irp );
						break;

	                //
		            //  For plug and play operations.
			        //

				    case IRP_MJ_PNP:

					    //
						//  I don't believe this should ever occur, but allow for the unexpected.
	                    //

		                (VOID) FatCommonPnp( IrpContext, Irp );
			            break;

				    //
					//  For any other major operations, return an invalid
	                //  request.
		            //

			        default:

				        FatCompleteRequest( IrpContext, Irp, STATUS_INVALID_DEVICE_REQUEST );
					    break;

				}

				if (volDoResourceAcquired) {

					SecondaryReleaseResourceLite( NULL, &VolDo->Resource );
					volDoResourceAcquired = FALSE;
				}

Next: NOTHING;

	        } except(FatExceptionFilter( IrpContext, GetExceptionInformation() )) {

				if (volDoResourceAcquired) {

					SecondaryReleaseResourceLite( NULL, &VolDo->Resource );
					volDoResourceAcquired = FALSE;
				}
				
				//
			    //  We had some trouble trying to perform the requested
				//  operation, so we'll abort the I/O request with
	            //  the error status that we get back from the
		        //  execption code.
			    //

				Status = FatProcessException( IrpContext, Irp, GetExceptionCode() );
			}
		
		} while (Status == STATUS_CANT_WAIT);

#else

        try {

            switch ( IrpContext->MajorFunction ) {

                //
                //  For Create Operation,
                //

                case IRP_MJ_CREATE:

                    (VOID) FatCommonCreate( IrpContext, Irp );
                    break;

                //
                //  For close operations.  We do a little kludge here in case
                //  this close causes a volume to go away.  It will NULL the
                //  VolDo local variable so that we will not try to look at
                //  the overflow queue.
                //

                case IRP_MJ_CLOSE:

                {
                    PVCB Vcb;
                    PFCB Fcb;
                    PCCB Ccb;
                    TYPE_OF_OPEN TypeOfOpen;

                    //
                    //  Extract and decode the file object
                    //

                    TypeOfOpen = FatDecodeFileObject( IrpSp->FileObject, &Vcb, &Fcb, &Ccb );

                    //
                    //  Do the close.  We have a slightly different format
                    //  for this call because of the async closes.
                    //

                    Status = FatCommonClose( Vcb,
                                             Fcb,
                                             Ccb,
                                             TypeOfOpen,
                                             TRUE,
                                             &VcbDeleted );

                    //
                    //  If the VCB was deleted, do not try to access it later.
                    //

                    if (VcbDeleted) {

                        VolDo = NULL;
                    }

                    ASSERT(Status == STATUS_SUCCESS);

                    FatCompleteRequest( IrpContext, Irp, Status );

                    break;
                }

                //
                //  For read operations
                //

                case IRP_MJ_READ:

                    (VOID) FatCommonRead( IrpContext, Irp );
                    break;

                //
                //  For write operations,
                //

                case IRP_MJ_WRITE:

                    (VOID) FatCommonWrite( IrpContext, Irp );
                    break;

                //
                //  For Query Information operations,
                //

                case IRP_MJ_QUERY_INFORMATION:

                    (VOID) FatCommonQueryInformation( IrpContext, Irp );
                    break;

                //
                //  For Set Information operations,
                //

                case IRP_MJ_SET_INFORMATION:

                    (VOID) FatCommonSetInformation( IrpContext, Irp );
                    break;

                //
                //  For Query EA operations,
                //

                case IRP_MJ_QUERY_EA:

                    (VOID) FatCommonQueryEa( IrpContext, Irp );
                    break;

                //
                //  For Set EA operations,
                //

                case IRP_MJ_SET_EA:

                    (VOID) FatCommonSetEa( IrpContext, Irp );
                    break;

                //
                //  For Flush buffers operations,
                //

                case IRP_MJ_FLUSH_BUFFERS:

                    (VOID) FatCommonFlushBuffers( IrpContext, Irp );
                    break;

                //
                //  For Query Volume Information operations,
                //

                case IRP_MJ_QUERY_VOLUME_INFORMATION:

                    (VOID) FatCommonQueryVolumeInfo( IrpContext, Irp );
                    break;

                //
                //  For Set Volume Information operations,
                //

                case IRP_MJ_SET_VOLUME_INFORMATION:

                    (VOID) FatCommonSetVolumeInfo( IrpContext, Irp );
                    break;

                //
                //  For File Cleanup operations,
                //

                case IRP_MJ_CLEANUP:

                    (VOID) FatCommonCleanup( IrpContext, Irp );
                    break;

                //
                //  For Directory Control operations,
                //

                case IRP_MJ_DIRECTORY_CONTROL:

                    (VOID) FatCommonDirectoryControl( IrpContext, Irp );
                    break;

                //
                //  For File System Control operations,
                //

                case IRP_MJ_FILE_SYSTEM_CONTROL:

                    (VOID) FatCommonFileSystemControl( IrpContext, Irp );
                    break;

                //
                //  For Lock Control operations,
                //

                case IRP_MJ_LOCK_CONTROL:

                    (VOID) FatCommonLockControl( IrpContext, Irp );
                    break;

                //
                //  For Device Control operations,
                //

                case IRP_MJ_DEVICE_CONTROL:

                    (VOID) FatCommonDeviceControl( IrpContext, Irp );
                    break;

                //
                //  For the Shutdown operation,
                //

                case IRP_MJ_SHUTDOWN:

                    (VOID) FatCommonShutdown( IrpContext, Irp );
                    break;

                //
                //  For plug and play operations.
                //

                case IRP_MJ_PNP:

                    //
                    //  I don't believe this should ever occur, but allow for the unexpected.
                    //

                    (VOID) FatCommonPnp( IrpContext, Irp );
                    break;

                //
                //  For any other major operations, return an invalid
                //  request.
                //

                default:

                    FatCompleteRequest( IrpContext, Irp, STATUS_INVALID_DEVICE_REQUEST );
                    break;

            }

        } except(FatExceptionFilter( IrpContext, GetExceptionInformation() )) {

            //
            //  We had some trouble trying to perform the requested
            //  operation, so we'll abort the I/O request with
            //  the error status that we get back from the
            //  execption code.
            //

            (VOID) FatProcessException( IrpContext, Irp, GetExceptionCode() );
        }
#endif

        IoSetTopLevelIrp( NULL );

        FsRtlExitFileSystem();

        //
        //  If there are any entries on this volume's overflow queue, service
        //  them.
        //

        if ( VolDo != NULL ) {

            PVOID Entry;

            //
            //  We have a volume device object so see if there is any work
            //  left to do in its overflow queue.
            //

            Entry = FatRemoveOverflowEntry( VolDo );

            //
            //  There wasn't an entry, break out of the loop and return to
            //  the Ex Worker thread.
            //

            if ( Entry == NULL ) {

                break;
            }

            //
            //  Extract the IrpContext, Irp, and IrpSp, and loop.
            //

            IrpContext = CONTAINING_RECORD( Entry,
                                            IRP_CONTEXT,
                                            WorkQueueItem.List );

            SetFlag(IrpContext->Flags, IRP_CONTEXT_FLAG_WAIT | IRP_CONTEXT_FLAG_IN_FSP);

            Irp = IrpContext->OriginatingIrp;

            IrpSp = IoGetCurrentIrpStackLocation( Irp );

            continue;

        } else {

            break;
        }
    }

    //
    //  Decrement the PostedRequestCount.
    //

    if ( VolDo ) {

        ExInterlockedAddUlong( &VolDo->PostedRequestCount,
                               0xffffffff,
                               &VolDo->OverflowQueueSpinLock );
    }

    return;
}


//
//  Internal support routine, spinlock wrapper.
//

PVOID
FatRemoveOverflowEntry (
    IN PVOLUME_DEVICE_OBJECT VolDo
    )
{
    PVOID Entry;
    KIRQL SavedIrql;

    KeAcquireSpinLock( &VolDo->OverflowQueueSpinLock, &SavedIrql );

    if (VolDo->OverflowQueueCount > 0) {

        //
        //  There is overflow work to do in this volume so we'll
        //  decrement the Overflow count, dequeue the IRP, and release
        //  the Event
        //

        VolDo->OverflowQueueCount -= 1;

        Entry = RemoveHeadList( &VolDo->OverflowQueue );

    } else {

        Entry = NULL;
    }

    KeReleaseSpinLock( &VolDo->OverflowQueueSpinLock, SavedIrql );

    return Entry;
}


