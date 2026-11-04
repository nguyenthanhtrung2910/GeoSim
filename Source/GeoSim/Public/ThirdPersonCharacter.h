/* Copyright 2022 Esri
*
 * Licensed under the Apache License Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "InputActionValue.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "EnhancedInputSubsystems.h"
#include "ThirdPersonCharacter.generated.h"

UCLASS()
class GEOSIM_API AThirdPersonCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AThirdPersonCharacter();
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
		USpringArmComponent* CameraBoom;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
		UCameraComponent* FollowCamera;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enhanced Input")
		UInputMappingContext* MappingContext;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enhanced Input")
		UInputAction* JumpAction;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enhanced Input")
		UInputAction* MoveForwardAction;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enhanced Input")
		UInputAction* MoveRightAction;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enhanced Input")
		 UInputAction* LookAction;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enhanced Input")
		UInputAction* MoveUpAction;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enhanced Input")
		UInputAction* StartFlyingAction;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Enhanced Input")
		UInputAction* SprintingAction;
	
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginPlay() override;


private:
	bool bSprinting;
	bool bFlying;

protected:

private:	
	void SetCameraBoomSettings() const;
	void StartFlying(const FInputActionValue& value);
	void JumpActionEvent(const FInputActionValue& value);
	void StopJumpActionEvent(const FInputActionValue& value);
	void Look(const FInputActionValue& Value);
	void MoveUp(const FInputActionValue& Value);
	void MoveForward(const FInputActionValue& Value);
	void MoveRight(const FInputActionValue& Value);
	void Sprint(const FInputActionValue& Value);

public:
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
};
