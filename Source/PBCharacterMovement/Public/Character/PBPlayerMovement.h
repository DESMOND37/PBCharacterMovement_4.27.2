// Copyright Project Borealis
// UE4.27.2 port — based on v3.1.0 (features) + v2.0.1 (UE4 API compatibility)

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PBPlayerCharacter.h"
#include "Runtime/Launch/Resources/Version.h"

#include "PBPlayerMovement.generated.h"

// UE4 port: Using #define to match 2.0.1 style (constexpr float also works in UE4 C++14)
#define LADDER_MOUNT_TIMEOUT 0.2f

// Crouch Timings (in seconds)
#define MOVEMENT_DEFAULT_CROUCHTIME        0.4f
#define MOVEMENT_DEFAULT_CROUCHJUMPTIME    0.0f
#define MOVEMENT_DEFAULT_UNCROUCHTIME      0.2f
#define MOVEMENT_DEFAULT_UNCROUCHJUMPTIME  0.8f

class USoundCue;

constexpr float DesiredGravity = -1143.0f;

UCLASS()
class PBCHARACTERMOVEMENT_API UPBPlayerMovement : public UCharacterMovementComponent
{
	GENERATED_BODY()

protected:
	/** If the player is using a ladder */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = Gameplay)
	bool bOnLadder;

	/** Should crouch slide? */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	bool bShouldCrouchSlide;

	/** If the player is currently crouch sliding */
	UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Category = Gameplay)
	bool bCrouchSliding;

	/** Schedule a crouch slide to landing */
	bool bDeferCrouchSlideToLand;

	/** Time crouch sliding started */
	float CrouchSlideStartTime;

	/** How long a crouch slide boosts for */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float CrouchSlideBoostTime;

	/** The minimum starting boost */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float MinCrouchSlideBoost;

	/** Factor for determining the initial crouch slide boost up a slope */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float CrouchSlideBoostSlopeFactor;

	/** How much to multiply initial velocity by when starting a crouch slide */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float CrouchSlideBoostMultiplier;

	/** How much forward velocity player needs relative to sprint speed to start a crouch slide */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float CrouchSlideSpeedRequirementMultiplier;

	/** Max velocity multiplier for acceleration in crouch sliding */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float MaxCrouchSlideVelocityBoost;

	/** Min velocity multiplier for acceleration in crouch sliding */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float MinCrouchSlideVelocityBoost;

	/** Time before being able to crouch slide again */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float CrouchSlideCooldown;

	/** Enter crouch slide mode */
	void StartCrouchSlide();
	/** Check if crouch slide should be active */
	bool ShouldCrouchSlide() const;

	/** The time that the player can remount on the ladder */
	float OffLadderTicks = -1.0f;

	/** Acceleration multiplier when on ground */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float GroundAccelerationMultiplier;

	/** Acceleration multiplier when in air */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Jumping / Falling")
	float AirAccelerationMultiplier;

	/** Air speed cap (non-sliding) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Jumping / Falling")
	float AirSpeedCap;

	/** Air speed cap when sliding in air */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Jumping / Falling")
	float AirSlideSpeedCap;

	/** Proportion of input acceleration used for horizontal air dash (0 = disabled) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Jumping / Falling")
	float AirJumpDashMagnitude = 0.0f;

	/** If an air jump resets all horizontal movement */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Jumping / Falling")
	bool bAirJumpResetsHorizontal = false;

	/** Time to crouch on ground (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float CrouchTime;

	/** Time to uncrouch on ground (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float UncrouchTime;

	/** Time to crouch in air (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float CrouchJumpTime;

	/** Time to uncrouch in air (seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float UncrouchJumpTime;

	/** Speed on a ladder */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Ladder")
	float LadderSpeed;

	/** Ladder timeout */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Ladder")
	float LadderTimeout;

	/** Minimum step height when moving fast */
	UPROPERTY(Category = "Character Movement: Walking", EditAnywhere, BlueprintReadWrite)
	float MinStepHeight;

	/** Fraction of MaxStepHeight to use for step down */
	UPROPERTY(Category = "Character Movement: Walking", EditAnywhere, BlueprintReadWrite)
	float StepDownHeightFraction;

	/** Friction multiplier when on an edge */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float EdgeFrictionMultiplier;

	/** Height away from a floor to apply edge friction */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float EdgeFrictionHeight;

	/** Distance ahead to look for edges */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float EdgeFrictionDist;

	/** Only apply edge friction when braking */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	bool bEdgeFrictionOnlyWhenBraking;

	/** Always apply edge friction when crouching */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Walking")
	float bEdgeFrictionAlwaysWhenCrouching;

	/** Time (seconds) after landing before braking friction is applied */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement: Jumping / Falling")
	float BrakingWindow;

	/** Time elapsed in current braking window (seconds) */
	float BrakingWindowTimeElapsed;

	/** True once player has been on ground long enough for braking to apply */
	bool bBrakingFrameTolerated;

	/** Wait a frame before applying crouch speed cap */
	bool bCrouchFrameTolerated;

	/** Currently in a crouch transition */
	bool bIsInCrouchTransition;

	/** Player is locked into crouch state */
	bool bLockInCrouch = false;

	APBPlayerCharacter* GetPBCharacter() const { return PBPlayerCharacter; }

	/**
	 * UE4 port: TObjectPtr<APBPlayerCharacter> is UE5-only.
	 * Raw pointer used instead — functionally identical in UE4.
	 */
	UPROPERTY(Transient, DuplicateTransient)
	APBPlayerCharacter* PBPlayerCharacter;

	/** Ground run speed */
	UPROPERTY(Category = "Character Movement: Walking", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", UIMin = "0"))
	float RunSpeed;

	/** Ground sprint speed */
	UPROPERTY(Category = "Character Movement: Walking", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", UIMin = "0"))
	float SprintSpeed;

	/** Slow walk speed */
	UPROPERTY(Category = "Character Movement: Walking", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", UIMin = "0"))
	float WalkSpeed;

	/** Minimum speed for slope movement scaling */
	UPROPERTY(Category = "Character Movement: Walking", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", UIMin = "0"))
	float SpeedMultMin;

	/** Maximum speed for slope movement scaling */
	UPROPERTY(Category = "Character Movement: Walking", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", UIMin = "0"))
	float SpeedMultMax;

	/** Max camera roll angle */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement (General Settings)")
	float RollAngle;

	/** Camera roll speed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement (General Settings)")
	float RollSpeed;

	/** Bounce multiplier for slope deflection */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement (General Settings)")
	float BounceMultiplier = 0.0f;

	UPROPERTY(Category = "Character Movement: Walking", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", UIMin = "0"))
	float AxisSpeedLimit;

	/** Threshold for catching air on slopes */
	UPROPERTY(Category = "Character Movement: Walking", EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", UIMin = "0"))
	float SlideLimit = 0.5f;

	/** Fraction of uncrouch height to check before uncrouching */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement (General Settings)")
	float GroundUncrouchCheckFactor = 0.75f;

	bool bShouldPlayMoveSounds = true;

	/** Milliseconds between step sounds */
	float MoveSoundTime = 0.0f;
	/** If we are stepping left (else right) */
	bool StepSide = false;

	/** Plays footstep sound based on movement and surface */
	virtual void PlayMoveSound(float DeltaTime);
	virtual void PlayJumpSound(const FHitResult& Hit, bool bJumped);
	UPBMoveStepSound* GetMoveStepSoundBySurface(EPhysicalSurface SurfaceType) const;

public:
	/** Print pos and vel (Source: cl_showpos) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Movement (General Settings)")
	uint32 bShowPos : 1;

	UPBPlayerMovement();

	virtual void InitializeComponent() override;
	virtual void OnRegister() override;

	// Source-like movement overrides
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration) override;
	virtual void ApplyVelocityBraking(float DeltaTime, float Friction, float BrakingDeceleration) override;
	bool ShouldLimitAirControl(float DeltaTime, const FVector& FallAcceleration) const override;
	FVector NewFallVelocity(const FVector& InitialVelocity, const FVector& Gravity, float DeltaTime) const override;

	/**
	 * UE4 port: Custom PhysFalling override from 2.0.1.
	 * Ensures correct jump apex sub-stepping on UE4's physics model.
	 * UE5 (3.0+) dropped this override as the engine handled it natively.
	 */
	virtual void PhysFalling(float DeltaTime, int32 Iterations) override;

	void UpdateCharacterStateBeforeMovement(float DeltaSeconds) override;
	void UpdateCharacterStateAfterMovement(float DeltaSeconds) override;

	void UpdateSurfaceFriction(bool bIsSliding = false);
	void UpdateCrouching(float DeltaTime, bool bOnlyUnCrouch = false);

	// Crouch transition overrides
	virtual void Crouch(bool bClientSimulation = false) override;
	virtual void UnCrouch(bool bClientSimulation = false) override;
	virtual void DoCrouchResize(float TargetTime, float DeltaTime, bool bClientSimulation = false);
	virtual void DoUnCrouchResize(float TargetTime, float DeltaTime, bool bClientSimulation = false);

	bool MoveUpdatedComponentImpl(const FVector& Delta, const FQuat& NewRotation, bool bSweep, FHitResult* OutHit = nullptr, ETeleportType Teleport = ETeleportType::None) override;

	// Jump overrides
	bool CanAttemptJump() const override;
	bool DoJump(bool bClientSimulation) override;

	float GetFallSpeed(bool bAfterLand = false);

	/** Exit crouch slide mode */
	void StopCrouchSliding();

	/** Toggle crouch lock */
	void ToggleCrouchLock(bool bLock);

	void TwoWallAdjust(FVector& OutDelta, const FHitResult& Hit, const FVector& OldHitNormal) const override;
	float SlideAlongSurface(const FVector& Delta, float Time, const FVector& Normal, FHitResult& Hit, bool bHandleImpact = false) override;
	FVector ComputeSlideVector(const FVector& Delta, const float Time, const FVector& Normal, const FHitResult& Hit) const override;
	FVector HandleSlopeBoosting(const FVector& SlideResult, const FVector& Delta, const float Time, const FVector& Normal, const FHitResult& Hit) const override;
	bool ShouldCatchAir(const FFindFloorResult& OldFloor, const FFindFloorResult& NewFloor) override;
	bool IsWithinEdgeTolerance(const FVector& CapsuleLocation, const FVector& TestImpactPoint, const float CapsuleRadius) const override;
	bool ShouldCheckForValidLandingSpot(float DeltaTime, const FVector& Delta, const FHitResult& Hit) const override;
	void HandleImpact(const FHitResult& Hit, float TimeSlice = 0.0f, const FVector& MoveDelta = FVector::ZeroVector) override;
	bool IsValidLandingSpot(const FVector& CapsuleLocation, const FHitResult& Hit) const override;

	/** Blueprint event fired when player performs an air jump */
	UFUNCTION(BlueprintImplementableEvent)
	void OnAirJump(int32 JumpTimes);

	float GetFrictionFromHit(const FHitResult& Hit) const;
	void TraceCharacterFloor(FHitResult& OutHit) const;
	void TraceLineToFloor(FHitResult& OutHit) const;

	FORCEINLINE FVector GetAcceleration() const { return Acceleration; }
	FORCEINLINE bool GetCrouchLocked() const { return bLockInCrouch; }

	float GetSprintSpeed() const { return SprintSpeed; }

	void OnMovementModeChanged(EMovementMode PreviousMovementMode, uint8 PreviousCustomMode) override;

	float GetCameraRoll();

	UFUNCTION(BlueprintCallable)
	bool IsOnLadder() const;

	float GetLadderClimbSpeed() const;

	void SetNoClip(bool bNoClip);
	void ToggleNoClip();

	bool IsBrakingFrameTolerated() const { return bBrakingFrameTolerated; }
	bool IsInCrouchTransition() const { return bIsInCrouchTransition; }
	bool IsCrouchSliding() const { return bCrouchSliding; }
	void SetShouldPlayMoveSounds(bool bShouldPlay) { bShouldPlayMoveSounds = bShouldPlay; }

	virtual float GetMaxSpeed() const override;
	virtual void ApplyDownwardForce(float DeltaSeconds) override;

private:
	float DefaultStepHeight;
	float DefaultSpeedMultMin;
	float DefaultSpeedMultMax;
	float DefaultWalkableFloorZ;
	float SurfaceFriction;
	TWeakObjectPtr<UPrimitiveComponent> OldBase;

	bool bHasEverLanded = false;
	bool bSlidingInAir = false;
	bool bWasSlidingInAir = false;
	bool bHasDeferredMovementMode;
	EMovementMode DeferredMovementMode;
};
